/*
 *	The PCI Library -- Hurd access via RPCs
 *
 *	Copyright (c) 2017 Joan Lledó <jlledom@member.fsf.org>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#define _GNU_SOURCE

#include "internal.h"

#include <string.h>
#include <hurd.h>
#include <hurd/pci_conf.h>
#include <hurd/paths.h>

/* Server port */
mach_port_t pci_server_port = MACH_PORT_NULL;

/* Get the server port */
static int
hurd_detect(struct pci_access *a)
{
  int res;

  pci_server_port = file_name_lookup(_SERVERS_PCI_CONF, 0, 0);

  if (pci_server_port == MACH_PORT_NULL)
    {
      a->error("Could not open file `%s'", _SERVERS_PCI_CONF);
      res = 0;
    }
  else
    res = 1;

  return res;
}

static void
hurd_init(struct pci_access *a UNUSED)
{
}

static void
hurd_cleanup(struct pci_access *a UNUSED)
{
}

static int
hurd_read(struct pci_dev *d, int pos, byte *buf, int len)
{
  int err;
  size_t nread;
  char *data;

  nread = len;
  if(len > 4)
    err = !pci_generic_block_read(d, pos, buf, nread);
  else
    {
      data = (char*)buf;
      err = pci_conf_read(pci_server_port, d->bus, d->dev, d->func, pos, &data,
                          &nread, len);

      if (data != (char*)buf)
        {
          if (nread > (size_t)len)	/* Sanity check for bogus server.  */
            {
              vm_deallocate(mach_task_self(), (vm_address_t)data, nread);
              return 0;
            }

          memcpy(buf, data, nread);
          vm_deallocate(mach_task_self(), (vm_address_t)data, nread);
        }
    }
  if (err)
    return 0;

  return nread == (size_t)len;
}

static int
hurd_write(struct pci_dev *d, int pos, byte *buf, int len)
{
  int err;
  size_t nwrote;

  nwrote = len;
  if(len > 4)
    err = !pci_generic_block_write(d, pos, buf, len);
  else
    err = pci_conf_write(pci_server_port, d->bus, d->dev, d->func, pos,
                         (char*)buf, len, &nwrote);
  if (err)
    return 0;

  return nwrote == (size_t)len;
}

struct pci_methods pm_hurd = {
  "hurd",
  "Hurd access using RPCs",
  NULL,					/* config */
  hurd_detect,
  hurd_init,
  hurd_cleanup,
  pci_generic_scan,
  pci_generic_fill_info,
  hurd_read,
  hurd_write,
  NULL,					/* read_vpd */
  NULL,					/* init_dev */
  NULL					/* cleanup_dev */
};
