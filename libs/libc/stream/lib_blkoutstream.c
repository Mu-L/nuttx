/****************************************************************************
 * libs/libc/stream/lib_blkoutstream.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <unistd.h>
#include <nuttx/streams.h>

#include "libc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_DISABLE_MOUNTPOINT

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: blkoutstream_flush
 ****************************************************************************/

static int blkoutstream_flush(FAR struct lib_sostream_s *self)
{
  FAR struct lib_blkoutstream_s *stream =
                                 (FAR struct lib_blkoutstream_s *)self;
  size_t sectorsize = stream->geo.geo_sectorsize;
  int ret = OK;

  if (self->nput % sectorsize > 0)
    {
      ret = stream->inode->u.i_bops->write(stream->inode, stream->cache,
                                           self->nput / sectorsize, 1);
    }

  return ret;
}

/****************************************************************************
 * Name: blkoutstream_seek
 ****************************************************************************/

static off_t blkoutstream_seek(FAR struct lib_sostream_s *self,
                               off_t offset, int whence)
{
  FAR struct lib_blkoutstream_s *stream =
                                 (FAR struct lib_blkoutstream_s *)self;
  size_t sectorsize = stream->geo.geo_sectorsize;
  off_t streamsize = sectorsize * stream->geo.geo_nsectors;
  FAR struct inode *inode = stream->inode;
  off_t sector;
  off_t ret;

  switch (whence)
    {
      case SEEK_SET:
        break;
      case SEEK_END:
        offset += streamsize;
        break;
      case SEEK_CUR:
        offset += self->nput;
        break;
      default:
        return -ENOTSUP;
    }

  /* Seek to negative value or value larger than maximum size shall fail. */

  if (offset < 0 || offset > streamsize)
    {
      return -EINVAL;
    }

  if (self->nput % sectorsize)
    {
      sector = self->nput / sectorsize;
      if (offset >= sector * sectorsize &&
          offset < (sector + 1) * sectorsize)
        {
          /* Inside same sector */

          goto out;
        }

      ret = inode->u.i_bops->write(stream->inode, stream->cache,
                                   sector, 1);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (offset % sectorsize)
    {
      ret = inode->u.i_bops->read(inode, stream->cache,
                                  offset / sectorsize, 1);
      if (ret < 0)
        {
          return ret;
        }
    }

out:
  self->nput = offset;
  return offset;
}

/****************************************************************************
 * Name: blkoutstream_puts
 ****************************************************************************/

static ssize_t blkoutstream_puts(FAR struct lib_sostream_s *self,
                                 FAR const void *buf, size_t len)
{
  FAR struct lib_blkoutstream_s *stream =
                                 (FAR struct lib_blkoutstream_s *)self;
  size_t sectorsize = stream->geo.geo_sectorsize;
  FAR struct inode *inode = stream->inode;
  FAR const unsigned char *ptr = buf;
  size_t remain = len;
  ssize_t ret;

  while (remain > 0)
    {
      off_t sector = self->nput / sectorsize;
      off_t offset = self->nput % sectorsize;

      if (offset > 0)
        {
          size_t copying = offset + remain > sectorsize ?
                           sectorsize - offset : remain;

          memcpy(stream->cache + offset, ptr, copying);

          ptr        += copying;
          offset     += copying;
          self->nput += copying;
          remain     -= copying;

          if (offset == sectorsize)
            {
              ret = inode->u.i_bops->write(inode, stream->cache, sector, 1);
              if (ret < 0)
                {
                  return ret;
                }
            }
        }
      else if (remain < sectorsize)
        {
          /* Read sector back to keep as more as possible old data */

          ret = inode->u.i_bops->read(inode, stream->cache, sector, 1);
          if (ret < 0)
            {
              return ret;
            }

          memcpy(stream->cache, ptr, remain);
          self->nput += remain;
          remain      = 0;
        }
      else
        {
          size_t nsector = remain / sectorsize;
          size_t copying = nsector * sectorsize;

          ret = inode->u.i_bops->write(inode, ptr, sector, nsector);
          if (ret < 0)
            {
              return ret;
            }

          ptr        += copying;
          self->nput += copying;
          remain     -= copying;
        }
    }

  return len;
}

/****************************************************************************
 * Name: blkoutstream_putc
 ****************************************************************************/

static void blkoutstream_putc(FAR struct lib_sostream_s *self, int ch)
{
  char tmp = ch;
  blkoutstream_puts(self, &tmp, 1);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: lib_blkoutstream_close
 *
 * Description:
 *  close block driver stream backend
 *
 * Input Parameters:
 *   stream  - User allocated, uninitialized instance of struct
 *                lib_blkoutstream_s to be initialized.
 *
 * Returned Value:
 *   None (User allocated instance initialized).
 *
 ****************************************************************************/

void lib_blkoutstream_close(FAR struct lib_blkoutstream_s *stream)
{
  if (stream != NULL)
    {
      if (stream->cache != NULL)
        {
          blkoutstream_flush(&stream->common);
          lib_free(stream->cache);
          stream->cache = NULL;
        }

      if (stream->inode != NULL)
        {
          close_blockdriver(stream->inode);
          stream->inode = NULL;
        }
    }
}

/****************************************************************************
 * Name: lib_blkoutstream_open
 *
 * Description:
 *  block driver stream backend
 *
 * Input Parameters:
 *   stream  - User allocated, uninitialized instance of struct
 *                lib_blkoutstream_s to be initialized.
 *   name    - The full path to the block driver to be opened.
 *
 * Returned Value:
 *   Returns zero on success or a negated errno on failure
 *
 ****************************************************************************/

int lib_blkoutstream_open(FAR struct lib_blkoutstream_s *stream,
                          FAR const char *name)
{
  FAR struct inode *inode = NULL;
  int ret;

  if (stream == NULL || name == NULL)
    {
      return -EINVAL;
    }

  ret = open_blockdriver(name, 0, &inode);
  if (ret < 0)
    {
      return ret;
    }

  memset(stream, 0, sizeof(*stream));

  if (inode->u.i_bops->geometry == NULL ||
      inode->u.i_bops->write == NULL ||
      inode->u.i_bops->geometry(inode, &stream->geo) < 0 ||
      stream->geo.geo_sectorsize <= 0 ||
      stream->geo.geo_nsectors <= 0)
    {
      close_blockdriver(inode);
      return -EINVAL;
    }

  stream->cache = lib_malloc(stream->geo.geo_sectorsize);
  if (stream->cache == NULL)
    {
      close_blockdriver(inode);
      return -ENOMEM;
    }

  stream->inode        = inode;
  stream->common.putc  = blkoutstream_putc;
  stream->common.puts  = blkoutstream_puts;
  stream->common.flush = blkoutstream_flush;
  stream->common.seek  = blkoutstream_seek;

  return OK;
}
#endif
