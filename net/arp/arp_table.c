/****************************************************************************
 * net/arp/arp_table.c
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 *   Copyright (C) 2007-2009, 2011, 2014, 2018 Gregory Nutt. All rights
 *     reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Based originally on uIP which also has a BSD style license:
 *
 *   Author: Adam Dunkels <adam@dunkels.com>
 *   Copyright (c) 2001-2003, Adam Dunkels.
 *   All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#ifdef CONFIG_NET

#include <sys/ioctl.h>
#include <stdint.h>
#include <string.h>
#include <debug.h>

#include <netinet/in.h>
#include <net/ethernet.h>

#include <nuttx/clock.h>
#include <nuttx/net/netconfig.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/ip.h>

#include "netdev/netdev.h"
#include "netlink/netlink.h"
#include "arp/arp.h"

#ifdef CONFIG_NET_ARP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ARP_MAXAGE_TICK SEC2TICK(10 * CONFIG_NET_ARP_MAXAGE)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct arp_table_info_s
{
  in_addr_t    ai_ipaddr;   /* IP address for lookup */
  FAR uint8_t *ai_ethaddr;  /* Location to return the MAC address */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The table of known address mappings */

static struct arp_entry_s g_arptable[CONFIG_NET_ARPTAB_SIZE];

static const struct ether_addr g_zero_ethaddr =
{
  {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  }
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arp_match
 *
 * Description:
 *   This is a callback that checks if the Ethernet network device has the
 *   indicated IPv4 address assigned to it.
 *
 ****************************************************************************/

static int arp_match(FAR struct net_driver_s *dev, FAR void *arg)
{
  FAR struct arp_table_info_s *info = arg;

  /* Make sure that this is an Ethernet device (or an IEEE 802.11 device
   * which is also Ethernet)
   */

  if (dev->d_lltype != NET_LL_ETHERNET &&
      dev->d_lltype != NET_LL_IEEE80211)
    {
      return 0;
    }

  /* Check if the network device has been assigned the IP address of the
   * lookup.
   */

  if (!net_ipv4addr_cmp(dev->d_ipaddr, info->ai_ipaddr))
    {
      return 0;
    }

  /* Yes.. Return the matching Ethernet MAC address if the caller of
   * arp_find() provided a non-NULL location.
   */

  if (info->ai_ethaddr != NULL)
    {
      memcpy(info->ai_ethaddr, &dev->d_mac.ether, ETHER_ADDR_LEN);
    }

  /* Return success in any event */

  return 1;
}

/****************************************************************************
 * Name: arp_return_old_entry
 *
 * Description:
 *   Compare and return the old ARP table entry.
 *
 ****************************************************************************/

static FAR struct arp_entry_s *
arp_return_old_entry(FAR struct arp_entry_s *e1, FAR struct arp_entry_s *e2)
{
  if (e1->at_ipaddr == 0)
    {
      return e1;
    }
  else if (e2->at_ipaddr == 0)
    {
      return e2;
    }
  else if ((int)(e1->at_time - e2->at_time) <= 0)
    {
      return e1;
    }
  else
    {
      return e2;
    }
}

/****************************************************************************
 * Name: arp_lookup
 *
 * Description:
 *   Find the ARP entry corresponding to this IP address in the ARP table.
 *
 * Input Parameters:
 *   ipaddr - Refers to an IP address in network order
 *   dev    - Device structure
 *   check_expiry - Expiry check
 *
 * Assumptions:
 *   The network is locked to assure exclusive access to the ARP table.
 *   The return value will become unstable when the network is unlocked.
 *
 ****************************************************************************/

static FAR struct arp_entry_s *arp_lookup(in_addr_t ipaddr,
                                          FAR struct net_driver_s *dev,
                                          bool check_expiry)
{
  FAR struct arp_entry_s *tabptr;
  int i;

  /* Check if the IPv4 address is already in the ARP table. */

  for (i = 0; i < CONFIG_NET_ARPTAB_SIZE; ++i)
    {
      tabptr = &g_arptable[i];
      if (tabptr->at_dev == dev &&
          net_ipv4addr_cmp(ipaddr, tabptr->at_ipaddr))
        {
          /* Find matching entries */

          if (!check_expiry)
            {
              return tabptr;  /* Ignore expiration time */
            }

          /* Check if it has expired */

          if (clock_systime_ticks() - tabptr->at_time <= ARP_MAXAGE_TICK)
            {
              return tabptr;
            }

          return NULL;  /* Expired */
        }
    }

  /* Not found */

  return NULL;
}

/****************************************************************************
 * Name: arp_get_arpreq
 *
 * Description:
 *   Translate (struct arp_entry_s) to (struct arpreq) for netlink notify.
 *
 * Input Parameters:
 *   output - Location to return the ARP table copy
 *   input  - The arp entry in table
 *
 ****************************************************************************/

#ifdef CONFIG_NETLINK_ROUTE
static void arp_get_arpreq(FAR struct arpreq *output,
                           FAR struct arp_entry_s *input)
{
  FAR struct sockaddr_in *outaddr;

  DEBUGASSERT(output != NULL && input != NULL);

  outaddr = (FAR struct sockaddr_in *)&output->arp_pa;
  outaddr->sin_family      = AF_INET;
  outaddr->sin_port        = 0;
  outaddr->sin_addr.s_addr = input->at_ipaddr;
  memcpy(output->arp_ha.sa_data, input->at_ethaddr.ether_addr_octet,
         sizeof(struct ether_addr));
  strlcpy(output->arp_dev, input->at_dev->d_ifname, sizeof(output->arp_dev));
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arp_update
 *
 * Description:
 *   Add the IP/HW address mapping to the ARP table -OR- change the IP
 *   address of an existing association.
 *
 * Input Parameters:
 *   dev     - The device driver structure
 *   ipaddr  - The IP address as an inaddr_t
 *   ethaddr - Refers to a HW address uint8_t[IFHWADDRLEN]
 *
 * Returned Value:
 *   Zero (OK) if the ARP table entry was successfully modified.  A negated
 *   errno value is returned on any error.
 *
 * Assumptions
 *   The network is locked to assure exclusive access to the ARP table
 *
 ****************************************************************************/

int arp_update(FAR struct net_driver_s *dev, in_addr_t ipaddr,
               FAR const uint8_t *ethaddr)
{
  FAR struct arp_entry_s *tabptr = &g_arptable[0];
#ifdef CONFIG_NETLINK_ROUTE
  struct arpreq arp_notify;
  bool found = false;
  bool new_entry;
#endif
  int i;

  /* Walk through the ARP mapping table and try to find an entry to
   * update. If none is found, the IP -> MAC address mapping is
   * inserted in the ARP table.
   */

  for (i = 0; i < CONFIG_NET_ARPTAB_SIZE; ++i)
    {
      /* Check if the source IP address of the incoming packet matches
       * the IP address in this ARP table entry.
       */

      if (g_arptable[i].at_dev == dev &&
          g_arptable[i].at_ipaddr != 0 &&
          net_ipv4addr_cmp(ipaddr, g_arptable[i].at_ipaddr))
        {
          /* An old entry found, break. */

          tabptr = &g_arptable[i];
#ifdef CONFIG_NETLINK_ROUTE
          found = true;
#endif
          break;
        }
      else
        {
          /* Record the oldest entry. */

          tabptr = arp_return_old_entry(tabptr, &g_arptable[i]);
        }
    }

  if (ethaddr == NULL)
    {
      ethaddr = g_zero_ethaddr.ether_addr_octet;
    }

  /* When overwrite old entry, notify old entry RTM_DELNEIGH */

#ifdef CONFIG_NETLINK_ROUTE
  if (!found && tabptr->at_ipaddr != 0)
    {
      arp_get_arpreq(&arp_notify, tabptr);
      netlink_neigh_notify(&arp_notify, RTM_DELNEIGH, AF_INET);
    }

  /* Need to notify when entry is not found or changes in table */

  new_entry = !found || memcmp(tabptr->at_ethaddr.ether_addr_octet,
                               ethaddr, ETHER_ADDR_LEN) != 0;
#endif

  /* Now, tabptr is the ARP table entry which we will fill with the new
   * information.
   */

  tabptr->at_ipaddr = ipaddr;
  memcpy(tabptr->at_ethaddr.ether_addr_octet, ethaddr, ETHER_ADDR_LEN);
  tabptr->at_dev = dev;
  tabptr->at_time = clock_systime_ticks();

  /* Notify the new entry */

#ifdef CONFIG_NETLINK_ROUTE
  if (new_entry)
    {
      arp_get_arpreq(&arp_notify, tabptr);
      netlink_neigh_notify(&arp_notify, RTM_NEWNEIGH, AF_INET);
    }
#endif

  return OK;
}

/****************************************************************************
 * Name: arp_hdr_update
 *
 * Description:
 *   Add the IP/HW address mapping to the ARP table -OR- change the IP
 *   address of an existing association.
 *
 * Input Parameters:
 *   dev     - The device driver structure
 *   pipaddr - Refers to an IP address uint16_t[2] in network order
 *   ethaddr - Refers to a HW address uint8_t[IFHWADDRLEN]
 *
 * Returned Value:
 *   Zero (OK) if the ARP table entry was successfully modified.  A negated
 *   errno value is returned on any error.
 *
 * Assumptions
 *   The network is locked to assure exclusive access to the ARP table
 *
 ****************************************************************************/

void arp_hdr_update(FAR struct net_driver_s *dev, FAR uint16_t *pipaddr,
                    FAR const uint8_t *ethaddr)
{
  in_addr_t ipaddr = net_ip4addr_conv32(pipaddr);

  /* Update the ARP table */

  arp_update(dev, ipaddr, ethaddr);
}

/****************************************************************************
 * Name: arp_find
 *
 * Description:
 *   Find the ARP entry corresponding to this IP address which may or may
 *   not be in the ARP table (it may, instead, be a local network device).
 *
 * Input Parameters:
 *   ipaddr  - Refers to an IP address in network order
 *   ethaddr - Location to return the corresponding Ethernet MAN address.
 *             This address may be NULL.  In that case, this function may be
 *             used simply to determine if the Ethernet MAC address is
 *             available.
 *   dev     - Device structure
 *   check_expiry  - Expiry check
 *
 * Assumptions
 *   The network is locked to assure exclusive access to the ARP table.
 *
 ****************************************************************************/

int arp_find(in_addr_t ipaddr, FAR uint8_t *ethaddr,
             FAR struct net_driver_s *dev, bool check_expiry)
{
  FAR struct arp_entry_s *tabptr;
  struct arp_table_info_s info;

  /* Check if the IPv4 address is already in the ARP table. */

  tabptr = arp_lookup(ipaddr, dev, check_expiry);
  if (tabptr != NULL)
    {
      /* Addresses that have failed to be searched will return a special
       * error code so that the upper layer can return faster.
       */

      if (memcmp(&tabptr->at_ethaddr, &g_zero_ethaddr,
                 sizeof(tabptr->at_ethaddr)) == 0)
        {
          return -ENETUNREACH;
        }

      /* Yes.. return the Ethernet MAC address if the caller has provided a
       * non-NULL address in 'ethaddr'.
       */

      if (ethaddr != NULL)
        {
          memcpy(ethaddr, &tabptr->at_ethaddr, ETHER_ADDR_LEN);
        }

      /* Return success meaning that a valid Ethernet MAC address mapping
       * is available for the IP address.
       */

      return OK;
    }

  /* No.. check if the IPv4 address is the address assigned to a local
   * Ethernet network device.  If so, return a mapping of that IP address
   * to the Ethernet MAC address assigned to the network device.
   */

  info.ai_ipaddr  = ipaddr;
  info.ai_ethaddr = ethaddr;

  if (netdev_foreach(arp_match, &info) != 0)
    {
      return OK;
    }

  /* Not found */

  return -ENOENT;
}

/****************************************************************************
 * Name: arp_delete
 *
 * Description:
 *   Remove an IP association from the ARP table
 *
 * Input Parameters:
 *   ipaddr - Refers to an IP address in network order
 *   dev    - Device structure
 *
 * Assumptions
 *   The network is locked to assure exclusive access to the ARP table.
 *
 ****************************************************************************/

int arp_delete(in_addr_t ipaddr, FAR struct net_driver_s *dev)
{
  FAR struct arp_entry_s *tabptr;
#ifdef CONFIG_NETLINK_ROUTE
  struct arpreq arp_notify;
#endif
  /* Check if the IPv4 address is in the ARP table. */

  tabptr = arp_lookup(ipaddr, dev, false);
  if (tabptr != NULL)
    {
      /* Notify to netlink */

#ifdef CONFIG_NETLINK_ROUTE
      arp_get_arpreq(&arp_notify, tabptr);
      netlink_neigh_notify(&arp_notify, RTM_DELNEIGH, AF_INET);
#endif

      /* Yes.. Set the IP address to zero to "delete" it */

      tabptr->at_ipaddr = 0;
      return OK;
    }

  return -ENOENT;
}

/****************************************************************************
 * Name: arp_cleanup
 *
 * Description:
 *   Clear the ARP table on the network device
 *
 * Input Parameters:
 *   dev  - The device driver structure
 *
 * Assumptions
 *   The network is locked to assure exclusive access to the ARP table.
 *
 ****************************************************************************/

void arp_cleanup(FAR struct net_driver_s *dev)
{
  int i;

  for (i = 0; i < CONFIG_NET_ARPTAB_SIZE; ++i)
    {
      if (dev == g_arptable[i].at_dev)
        {
          memset(&g_arptable[i], 0, sizeof(g_arptable[i]));
        }
    }
}

/****************************************************************************
 * Name: arp_snapshot
 *
 * Description:
 *   Take a snapshot of the current state of the ARP table.
 *
 * Input Parameters:
 *   snapshot  - Location to return the ARP table copy
 *   nentries  - The size of the user provided 'dest' in entries, each of
 *               size sizeof(struct arp_entry_s)
 *
 * Returned Value:
 *   On success, the number of entries actually copied is returned.  Unused
 *   entries are not returned.
 *
 * Assumptions
 *   The network is locked to assure exclusive access to the ARP table
 *
 ****************************************************************************/

#ifdef CONFIG_NETLINK_ROUTE
unsigned int arp_snapshot(FAR struct arpreq *snapshot,
                          unsigned int nentries)
{
  FAR struct arp_entry_s *tabptr;
  clock_t now;
  unsigned int ncopied;
  int i;

  /* Copy all non-empty, non-expired entries in the ARP table. */

  for (i = 0, now = clock_systime_ticks(), ncopied = 0;
       nentries > ncopied && i < CONFIG_NET_ARPTAB_SIZE;
       i++)
    {
      tabptr = &g_arptable[i];
      if (tabptr->at_ipaddr != 0 &&
          now - tabptr->at_time <= ARP_MAXAGE_TICK)
        {
          arp_get_arpreq(&snapshot[ncopied], tabptr);
          ncopied++;
        }
    }

  /* Return the number of entries copied into the user buffer */

  return ncopied;
}
#endif

#endif /* CONFIG_NET_ARP */
#endif /* CONFIG_NET */
