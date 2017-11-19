/*
 *	The PCI Library -- Hurd access via RPCs
 *
 *	Copyright (c) 2017 Joan Lledó <jlledom@member.fsf.org>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#define _GNU_SOURCE

#include "internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <hurd.h>
#include <hurd/pci_conf.h>
#include <hurd/paths.h>

/* Server path */
#define _SERVERS_PCI_CONF	_SERVERS_BUS "/pci"

/* Config file name */
#define FILE_CONFIG_NAME "config"

/* Level in the fs tree */
typedef enum
{
  LEVEL_NONE,
  LEVEL_DOMAIN,
  LEVEL_BUS,
  LEVEL_DEV,
  LEVEL_FUNC
} tree_level;

/* Check whether there's a pci server */
static int
hurd_detect (struct pci_access *a)
{
  int err;
  struct stat st;

  err = lstat (_SERVERS_PCI_CONF, &st);
  if (err)
    {
      a->error ("Could not open file `%s'", _SERVERS_PCI_CONF);
      return 0;
    }

  /* The node must be a directory and a translator */
  return S_ISDIR (st.st_mode) && ((st.st_mode & S_ITRANS) == S_IROOT);
}

/* Empty callbacks, we don't need any special init or cleanup */
static void
hurd_init (struct pci_access *a UNUSED)
{
}

static void
hurd_cleanup (struct pci_access *a UNUSED)
{
}

/* Each device has its own server path. Allocate space for the port. */
static void
hurd_init_dev (struct pci_dev *d)
{
  d->aux = calloc (1, sizeof (mach_port_t));
  assert (d->aux);
}

/* Deallocate the port and free its space */
static void
hurd_cleanup_dev (struct pci_dev *d)
{
  mach_port_t device_port;

  device_port = *((mach_port_t *) d->aux);
  mach_port_deallocate (mach_task_self (), device_port);

  free (d->aux);
}

/* Walk through the FS tree to see what is allowed for us */
static int
enum_devices (const char *parent, struct pci_access *a, int domain, int bus,
	      int dev, int func, tree_level lev)
{
  int err, ret, confd;
  DIR *dir;
  struct dirent *entry;
  char path[NAME_MAX];
  char server[NAME_MAX];
  uint32_t vd;
  uint8_t ht;
  mach_port_t device_port;
  struct pci_dev *d;

  dir = opendir (parent);
  if (!dir)
    return errno;

  while ((entry = readdir (dir)) != 0)
    {
      snprintf (path, NAME_MAX, "%s/%s", parent, entry->d_name);
      if (entry->d_type == DT_DIR)
	{
	  if (!strncmp (entry->d_name, ".", NAME_MAX)
	      || !strncmp (entry->d_name, "..", NAME_MAX))
	    continue;

	  errno = 0;
	  ret = strtol (entry->d_name, 0, 16);
	  if (errno)
	    return errno;

	  /*
	   * We found a valid directory.
	   * Update the address and switch to the next level.
	   */
	  switch (lev)
	    {
	    case LEVEL_DOMAIN:
	      domain = ret;
	      break;
	    case LEVEL_BUS:
	      bus = ret;
	      break;
	    case LEVEL_DEV:
	      dev = ret;
	      break;
	    case LEVEL_FUNC:
	      func = ret;
	      break;
	    default:
	      return -1;
	    }

	  err = enum_devices (path, a, domain, bus, dev, func, lev + 1);
	  if (err == EPERM)
	    continue;
	}
      else
	{
	  if (strncmp (entry->d_name, FILE_CONFIG_NAME, NAME_MAX))
	    /* We are looking for the config file */
	    continue;

	  /* We found an available virtual device, add it to our list */
	  confd = open (path, O_RDONLY, 0);
	  if (confd < 0)
	    return errno;

	  ret = lseek (confd, PCI_VENDOR_ID, SEEK_SET);
	  if (ret < 0)
	    return errno;
	  if (ret != PCI_VENDOR_ID)
	    return -1;
	  ret = read (confd, (char *) &vd, sizeof (vd));
	  if (ret < 0)
	    return errno;
	  if (ret != sizeof (vd))
	    return -1;

	  ret = lseek (confd, PCI_HEADER_TYPE, SEEK_SET);
	  if (ret < 0)
	    return errno;
	  if (ret != PCI_HEADER_TYPE)
	    return -1;
	  ret = read (confd, (char *) &ht, sizeof (ht));
	  if (ret < 0)
	    return errno;
	  if (ret != sizeof (ht))
	    return -1;

	  close (confd);

	  d = pci_alloc_dev (a);
	  d->bus = bus;
	  d->dev = dev;
	  d->func = func;
	  d->vendor_id = vd & 0xffff;
	  d->device_id = vd >> 16U;
	  d->known_fields = PCI_FILL_IDENT;
	  d->hdrtype = ht;
	  pci_link_dev (a, d);

	  snprintf (server, NAME_MAX, "%s/%04x/%02x/%02x/%01u",
		    _SERVERS_PCI_CONF, domain, bus, dev, func);
	  device_port = file_name_lookup (server, 0, 0);
	  if (device_port == MACH_PORT_NULL)
	    return errno;

	  *((mach_port_t *) d->aux) = device_port;
	}
    }

  return 0;
}

/* Enumerate devices */
static void
hurd_scan (struct pci_access *a)
{
  int err;

  err = enum_devices (_SERVERS_PCI_CONF, a, -1, -1, -1, -1, LEVEL_DOMAIN);
  assert (err == 0);
}

/*
 * Read `len' bytes to `buf'.
 *
 * Returns error when the number of read bytes does not match `len'.
 */
static int
hurd_read (struct pci_dev *d, int pos, byte * buf, int len)
{
  int err;
  size_t nread;
  char *data;
  mach_port_t device_port;

  nread = len;
  device_port = *((mach_port_t *) d->aux);
  if (len > 4)
    err = !pci_generic_block_read (d, pos, buf, nread);
  else
    {
      data = (char *) buf;
      err =
	pci_conf_read (device_port, d->bus, d->dev, d->func, pos, &data,
		       &nread, len);

      if (data != (char *) buf)
	{
	  if (nread > (size_t) len)	/* Sanity check for bogus server.  */
	    {
	      vm_deallocate (mach_task_self (), (vm_address_t) data, nread);
	      return 0;
	    }

	  memcpy (buf, data, nread);
	  vm_deallocate (mach_task_self (), (vm_address_t) data, nread);
	}
    }
  if (err)
    return 0;

  return nread == (size_t) len;
}

/*
 * Write `len' bytes from `buf'.
 *
 * Returns error when the number of written bytes does not match `len'.
 */
static int
hurd_write (struct pci_dev *d, int pos, byte * buf, int len)
{
  int err;
  size_t nwrote;
  mach_port_t device_port;

  nwrote = len;
  device_port = *((mach_port_t *) d->aux);
  if (len > 4)
    err = !pci_generic_block_write (d, pos, buf, len);
  else
    err = pci_conf_write (device_port, d->bus, d->dev, d->func, pos,
			  (char *) buf, len, &nwrote);
  if (err)
    return 0;

  return nwrote == (size_t) len;
}

struct pci_methods pm_hurd = {
  "hurd",
  "Hurd access using RPCs",
  NULL,				/* config */
  hurd_detect,
  hurd_init,
  hurd_cleanup,
  hurd_scan,
  pci_generic_fill_info,
  hurd_read,
  hurd_write,
  NULL,				/* read_vpd */
  hurd_init_dev,
  hurd_cleanup_dev
};
