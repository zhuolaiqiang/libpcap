/* -*- Mode: c; tab-width: 8; indent-tabs-mode: 1; c-basic-offset: 8; -*- */
/*
 * Copyright (c) 1994, 1995, 1996, 1997, 1998
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the Computer Systems
 *	Engineering Group at Lawrence Berkeley Laboratory.
 * 4. Neither the name of the University nor of the Laboratory may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef lint
static const char rcsid[] =
    "@(#) $Header: /tcpdump/master/libpcap/inet.c,v 1.39 2001-10-08 01:06:20 guy Exp $ (LBL)";
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/param.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_SOCKIO_H
#include <sys/sockio.h>
#endif
#include <sys/time.h>				/* concession to AIX */

struct mbuf;
struct rtentry;
#include <net/if.h>
#include <netinet/in.h>

#include <ctype.h>
#include <errno.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_IFADDRS_H
#include <ifaddrs.h>
#endif

#include "pcap-int.h"

#ifdef HAVE_OS_PROTO_H
#include "os-proto.h"
#endif

/* Not all systems have IFF_LOOPBACK */
#ifdef IFF_LOOPBACK
#define ISLOOPBACK(name, flags) ((flags) & IFF_LOOPBACK)
#else
#define ISLOOPBACK(name, flags) ((name)[0] == 'l' && (name)[1] == 'o' && \
    (isdigit((unsigned char)((name)[2])) || (name)[2] == '\0'))
#endif

/*
 * This is fun.
 *
 * In older BSD systems, socket addresses were fixed-length, and
 * "sizeof (struct sockaddr)" gave the size of the structure.
 * All addresses fit within a "struct sockaddr".
 *
 * In newer BSD systems, the socket address is variable-length, and
 * there's an "sa_len" field giving the length of the structure;
 * this allows socket addresses to be longer than 2 bytes of family
 * and 14 bytes of data.
 *
 * Some commercial UNIXes use the old BSD scheme, and some might use
 * the new BSD scheme.
 *
 * GNU libc uses neither scheme, but has an "SA_LEN()" macro that
 * determines the size based on the address family.
 */
#ifndef SA_LEN
#ifdef HAVE_SOCKADDR_SA_LEN
#define SA_LEN(addr)	((addr)->sa_len)
#else /* HAVE_SOCKADDR_SA_LEN */
#define SA_LEN(addr)	(sizeof (struct sockaddr))
#endif /* HAVE_SOCKADDR_SA_LEN */
#endif /* SA_LEN */

static struct sockaddr *
dup_sockaddr(struct sockaddr *sa)
{
	struct sockaddr *newsa;
	unsigned int size;
	
	size = SA_LEN(sa);
	if ((newsa = malloc(size)) == NULL)
		return (NULL);
	return (memcpy(newsa, sa, size));
}

static int
get_instance(char *name)
{
	char *cp, *endcp;
	int n;

	endcp = name + strlen(name);
	for (cp = name; cp < endcp && !isdigit((unsigned char)*cp); ++cp)
		continue;

	if (isdigit((unsigned char)*cp))
		n = atoi(cp);
	else
		n = 0;
	return (n);
}

static int
add_or_find_if(pcap_if_t **curdev_ret, pcap_if_t **alldevs, char *name,
    u_int flags, char *errbuf)
{
	pcap_t *p;
	pcap_if_t *curdev, *prevdev, *nextdev;
	int this_instance;

	/*
	 * Can we open this interface for live capture?
	 */
	p = pcap_open_live(name, 68, 0, 0, errbuf);
	if (p == NULL) {
		/*
		 * No.  Don't bother including it.
		 * Don't treat this as an error, though.
		 */
		*curdev_ret = NULL;
		return (0);
	}
	pcap_close(p);

	/*
	 * Is there already an entry in the list for this interface?
	 */
	for (curdev = *alldevs; curdev != NULL; curdev = curdev->next) {
		if (strcmp(name, curdev->name) == 0)
			break;	/* yes, we found it */
	}
	if (curdev == NULL) {
		/*
		 * No, we didn't find it.
		 * Allocate a new entry.
		 */
		curdev = malloc(sizeof(pcap_if_t));
		if (curdev == NULL) {
			(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
			    "malloc: %s", pcap_strerror(errno));
			return (-1);
		}
		
		/*
		 * Fill in the entry.
		 */
		curdev->next = NULL;
		curdev->name = malloc(strlen(name) + 1);
		strcpy(curdev->name, name);
		curdev->description = NULL;	/* not available */
		curdev->addresses = NULL;	/* list starts out as empty */
		curdev->is_loopback = ISLOOPBACK(name, flags);

		/*
		 * Add it to the list, in the appropriate location.
		 * First, get the instance number of this interface.
		 */
		this_instance = get_instance(name);

		/*
		 * Now look for the last interface with an instance number
		 * less than or equal to the new interface's instance
		 * number - except that non-loopback interfaces are
		 * arbitrarily treated as having interface numbers less
		 * than those of loopback interfaces, so the loopback
		 * interfaces are put at the end of the list.
		 *
		 * We start with "prevdev" being NULL, meaning we're before
		 * the first element in the list.
		 */
		prevdev = NULL;
		for (;;) {
			/*
			 * Get the interface after this one.
			 */
			if (prevdev == NULL) {
				/*
				 * The next element is the first element.
				 */
				nextdev = *alldevs;
			} else
				nextdev = prevdev->next;

			/*
			 * Are we at the end of the list?
			 */
			if (nextdev == NULL) {
				/*
				 * Yes - we have to put the new entry
				 * after "prevdev".
				 */
				break;
			}

			/*
			 * Is the new interface a non-loopback interface
			 * and the next interface a loopback interface?
			 */
			if (!curdev->is_loopback && nextdev->is_loopback) {
				/*
				 * Yes, we should put the new entry
				 * before "nextdev", i.e. after "prevdev".
				 */
				break;
			}

			/*
			 * Is the new interface's instance number less
			 * than the next interface's instance number,
			 * and is it the case that the new interface is a
			 * non-loopback interface or the next interface is
			 * a loopback interface?
			 *
			 * (The goal of both loopback tests is to make
			 * sure that we never put a loopback interface
			 * before any non-loopback interface and that we
			 * always put a non-loopback interface before all
			 * loopback interfaces.)
			 */
			if (this_instance < get_instance(nextdev->name) &&
			    (!curdev->is_loopback || nextdev->is_loopback)) {
				/*
				 * Yes - we should put the new entry
				 * before "nextdev", i.e. after "prevdev".
				 */
				break;
			}

			prevdev = nextdev;
		}

		/*
		 * Insert before "nextdev".
		 */
		curdev->next = nextdev;

		/*
		 * Insert after "prevdev" - unless "prevdev" is null,
		 * in which case this is the first interface.
		 */
		if (prevdev == NULL) {
			/*
			 * This is the first interface.  Pass back a
			 * pointer to it, and put "curdev" before
			 * "nextdev".
			 */
			*alldevs = curdev;
		} else
			prevdev->next = curdev;
	}
	
	*curdev_ret = curdev;
	return (0);
}

static int
add_addr_to_iflist(pcap_if_t **alldevs, char *name, u_int flags,
    struct sockaddr *addr, struct sockaddr *netmask,
    struct sockaddr *broadaddr, struct sockaddr *dstaddr, char *errbuf)
{
	pcap_if_t *curdev;
	pcap_addr_t *curaddr, *prevaddr, *nextaddr;

	if (add_or_find_if(&curdev, alldevs, name, flags, errbuf) == -1) {
		/*
		 * Error - give up.
		 */
		return (-1);
	}
	if (curdev == NULL) {
		/*
		 * Device wasn't added because it can't be opened.
		 * Not a fatal error.
		 */
		return (0);
	}

	/*
	 * "curdev" is an entry for this interface; add an entry for this
	 * address to its list of addresses.
	 *
	 * Allocate the new entry and fill it in.
	 */
	curaddr = malloc(sizeof(pcap_addr_t));
	if (curaddr == NULL) {
		(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
		    "malloc: %s", pcap_strerror(errno));
		return (-1);
	}

	curaddr->next = NULL;
	if (addr != NULL) {
		curaddr->addr = dup_sockaddr(addr);
		if (curaddr->addr == NULL) {
			(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
			    "malloc: %s", pcap_strerror(errno));
			free(curaddr);
			return (-1);
		}
	} else
		curaddr->addr = NULL;

	if (netmask != NULL) {
		curaddr->netmask = dup_sockaddr(netmask);
		if (curaddr->netmask == NULL) {
			(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
			    "malloc: %s", pcap_strerror(errno));
			free(curaddr);
			return (-1);
		}
	} else
		curaddr->netmask = NULL;
		
	if (broadaddr != NULL) {
		curaddr->broadaddr = dup_sockaddr(broadaddr);
		if (curaddr->broadaddr == NULL) {
			(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
			    "malloc: %s", pcap_strerror(errno));
			free(curaddr);
			return (-1);
		}
	} else
		curaddr->broadaddr = NULL;
		
	if (dstaddr != NULL) {
		curaddr->dstaddr = dup_sockaddr(dstaddr);
		if (curaddr->dstaddr == NULL) {
			(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
			    "malloc: %s", pcap_strerror(errno));
			free(curaddr);
			return (-1);
		}
	} else
		curaddr->dstaddr = NULL;
		
	/*
	 * Find the end of the list of addresses.
	 */
	for (prevaddr = curdev->addresses; prevaddr != NULL; prevaddr = nextaddr) {
		nextaddr = prevaddr->next;
		if (nextaddr == NULL) {
			/*
			 * This is the end of the list.
			 */
			break;
		}
	}

	if (prevaddr == NULL) {
		/*
		 * The list was empty; this is the first member.
		 */
		curdev->addresses = curaddr;
	} else {
		/*
		 * "prevaddr" is the last member of the list; append
		 * this member to it.
		 */
		prevaddr->next = curaddr;
	}

	return (0);
}

static int
pcap_addanydev(pcap_if_t **devlist, char *errbuf)
{
	pcap_if_t *curdev;

	return (add_or_find_if(&curdev, devlist, "any", 0, errbuf));
}

/*
 * Get a list of all interfaces that are up and that we can open.
 * Returns -1 on error, 0 otherwise.
 * The list, as returned through "alldevsp", may be null if no interfaces
 * were up and could be opened.
 */
#ifdef HAVE_IFADDRS_H
int
pcap_findalldevs(alldevsp, errbuf)
	pcap_if_t **alldevsp;
	register char *errbuf;
{
	pcap_if_t *devlist = NULL;
	struct ifaddrs *ifap, *ifa;
	int ret = 0;


	/*
	 * Get the list of interface addresses.
	 */
	if (getifaddrs(&ifap) != 0) {
		(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
		    "getifaddrs: %s", pcap_strerror(errno));
		return (-1);
	}
	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		/*
		 * Is this interface up?
		 */
		if (!(ifa->ifa_flags & IFF_UP)) {
			/*
			 * No, so don't add it to the list.
			 */
			continue;
		}

		/*
		 * Add information for this address to the list.
		 */
		if (add_addr_to_iflist(&devlist, ifa->ifa_name,
		    ifa->ifa_flags, ifa->ifa_addr, ifa->ifa_netmask,
		    ifa->ifa_broadaddr, ifa->ifa_dstaddr, errbuf) < 0) {
			/*
			 * Oops, we had a fatal error.
			 * Free the list we've been constructing, and fail.
			 */
			if (devlist != NULL) {
				pcap_freealldevs(devlist);
				devlist = NULL;
			}
			ret = -1;
			break;
		}
	}

	freeifaddrs(ifap);

	if (pcap_addanydev(&devlist, errbuf) < 0) {
		if (devlist != NULL) {
			pcap_freealldevs(devlist);
			devlist = NULL;
		}
		return -1;
	}

	*alldevsp = devlist;
	return (ret);
}
#else /* HAVE_IFADDRS_H */
int
pcap_findalldevs(alldevsp, errbuf)
	pcap_if_t **alldevsp;
	register char *errbuf;
{
	pcap_if_t *devlist = NULL;
	register int fd;
	register struct ifreq *ifrp, *ifend, *ifnext;
	int n;
	struct ifconf ifc;
	char *buf = NULL;
	unsigned buf_size;
	struct ifreq ifrflags, ifrnetmask, ifrbroadaddr, ifrdstaddr;
	struct sockaddr *netmask, *broadaddr, *dstaddr;
	int ret = 0;

	/*
	 * Create a socket from which to fetch the list of interfaces.
	 *
	 * XXX - on Linux, SIOCGIFCONF reports only interfaces with
	 * IPv4 addresses; you need to read "/proc/net/dev" to get
	 * the names of all the interfaces.
	 */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
		    "socket: %s", pcap_strerror(errno));
		return (-1);
	}

	/*
	 * Start with an 8K buffer, and keep growing the buffer until
	 * we get the entire interface list or fail to get it for some
	 * reason other than EINVAL (which is presumed here to mean
	 * "buffer is too small").
	 */
	buf_size = 8192;
	for (;;) {
		buf = malloc(buf_size);
		if (buf == NULL) {
			(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
			    "malloc: %s", pcap_strerror(errno));
			return (-1);
		}

		ifc.ifc_len = buf_size;
		ifc.ifc_buf = buf;
		memset(buf, 0, buf_size);
		if (ioctl(fd, SIOCGIFCONF, (char *)&ifc) < 0
		    && errno != EINVAL) {
			(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
			    "SIOCGIFCONF: %s", pcap_strerror(errno));
			close(fd);
			free(buf);
			return (-1);
		}
		if (ifc.ifc_len < buf_size)
			break;
		free(buf);
		buf_size *= 2;
	}

	ifrp = (struct ifreq *)buf;
	ifend = (struct ifreq *)(buf + ifc.ifc_len);

	for (; ifrp < ifend; ifrp = ifnext) {
		n = SA_LEN(&ifrp->ifr_addr) + sizeof(ifrp->ifr_name);
		if (n < sizeof(*ifrp))
			ifnext = ifrp + 1;
		else
			ifnext = (struct ifreq *)((char *)ifrp + n);

		/*
		 * Get the flags for this interface, and skip it if it's
		 * not up.
		 */
		strncpy(ifrflags.ifr_name, ifrp->ifr_name,
		    sizeof(ifrflags.ifr_name));
		if (ioctl(fd, SIOCGIFFLAGS, (char *)&ifrflags) < 0) {
			if (errno == ENXIO)
				continue;
			(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
			    "SIOCGIFFLAGS: %.*s: %s",
			    (int)sizeof(ifrflags.ifr_name),
			    ifrflags.ifr_name,
			    pcap_strerror(errno));
			if (devlist != NULL) {
				pcap_freealldevs(devlist);
				devlist = NULL;
			}
			ret = -1;
			break;
		}
		if (!(ifrflags.ifr_flags & IFF_UP))
			continue;

		/*
		 * Get the netmask for this address on this interface.
		 */
		strncpy(ifrnetmask.ifr_name, ifrp->ifr_name,
		    sizeof(ifrnetmask.ifr_name));
		memcpy(&ifrnetmask.ifr_addr, &ifrp->ifr_addr,
		    sizeof(ifrnetmask.ifr_addr));
		if (ioctl(fd, SIOCGIFNETMASK, (char *)&ifrnetmask) < 0) {
			if (errno == EADDRNOTAVAIL) {
				/*
				 * Not available.
				 */
				netmask = NULL;
			} else {
				(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
				    "SIOCGIFNETMASK: %.*s: %s",
				    (int)sizeof(ifrnetmask.ifr_name),
				    ifrnetmask.ifr_name,
				    pcap_strerror(errno));
				if (devlist != NULL) {
					pcap_freealldevs(devlist);
					devlist = NULL;
				}
				ret = -1;
				break;
			}
		} else
			netmask = &ifrnetmask.ifr_addr;

		/*
		 * Get the broadcast address for this address on this
		 * interface (if any).
		 */
		if (ifrflags.ifr_flags & IFF_BROADCAST) {
			strncpy(ifrbroadaddr.ifr_name, ifrp->ifr_name,
			    sizeof(ifrbroadaddr.ifr_name));
			memcpy(&ifrbroadaddr.ifr_addr, &ifrp->ifr_addr,
			    sizeof(ifrbroadaddr.ifr_addr));
			if (ioctl(fd, SIOCGIFBRDADDR,
			    (char *)&ifrbroadaddr) < 0) {
				if (errno == EADDRNOTAVAIL) {
					/*
					 * Not available.
					 */
					broadaddr = NULL;
				} else {
					(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
					    "SIOCGIFBRDADDR: %.*s: %s",
					    (int)sizeof(ifrbroadaddr.ifr_name),
					    ifrbroadaddr.ifr_name,
					    pcap_strerror(errno));
					if (devlist != NULL) {
						pcap_freealldevs(devlist);
						devlist = NULL;
					}
					ret = -1;
					break;
				}
			} else
				broadaddr = &ifrbroadaddr.ifr_broadaddr;
		} else {
			/*
			 * Not a broadcast interface, so no broadcast
			 * address.
			 */
			broadaddr = NULL;
		}

		/*
		 * Get the destination address for this address on this
		 * interface (if any).
		 */
		if (ifrflags.ifr_flags & IFF_POINTOPOINT) {
			strncpy(ifrdstaddr.ifr_name, ifrp->ifr_name,
			    sizeof(ifrdstaddr.ifr_name));
			memcpy(&ifrdstaddr.ifr_addr, &ifrp->ifr_addr,
			    sizeof(ifrdstaddr.ifr_addr));
			if (ioctl(fd, SIOCGIFDSTADDR,
			    (char *)&ifrdstaddr) < 0) {
				if (errno == EADDRNOTAVAIL) {
					/*
					 * Not available.
					 */
					dstaddr = NULL;
				} else {
					(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
					    "SIOCGIFDSTADDR: %.*s: %s",
					    (int)sizeof(ifrdstaddr.ifr_name),
					    ifrdstaddr.ifr_name,
					    pcap_strerror(errno));
					if (devlist != NULL) {
						pcap_freealldevs(devlist);
						devlist = NULL;
					}
					ret = -1;
					break;
				}
			} else
				dstaddr = &ifrdstaddr.ifr_dstaddr;
		} else
			dstaddr = NULL;

		/*
		 * Add information for this address to the list.
		 */
		if (add_addr_to_iflist(&devlist, ifrp->ifr_name,
		    ifrflags.ifr_flags, &ifrp->ifr_addr,
		    netmask, broadaddr, dstaddr, errbuf) < 0) {
			/*
			 * Oops, we had a fatal error.
			 * Free the list we've been constructing, and fail.
			 */
			if (devlist != NULL) {
				pcap_freealldevs(devlist);
				devlist = NULL;
			}
			ret = -1;
			break;
		}
	}

	(void)close(fd);
	free(buf);

	if (pcap_addanydev(&devlist, errbuf) < 0) {
		if (devlist != NULL) {
			pcap_freealldevs(devlist);
			devlist = NULL;
		}
		return -1;
	}

	*alldevsp = devlist;
	return (ret);
}
#endif /* HAVE_IFADDRS_H */

/*
 * Free a list of interfaces.
 */
void
pcap_freealldevs(pcap_if_t *alldevs)
{
	pcap_if_t *curdev, *nextdev;
	pcap_addr_t *curaddr, *nextaddr;

	for (curdev = alldevs; curdev != NULL; curdev = nextdev) {
		nextdev = curdev->next;

		/*
		 * Free all addresses.
		 */
		for (curaddr = curdev->addresses; curaddr != NULL; curaddr = nextaddr) {
			nextaddr = curaddr->next;
			if (curaddr->addr)
				free(curaddr->addr);
			if (curaddr->netmask)
				free(curaddr->netmask);
			if (curaddr->broadaddr)
				free(curaddr->broadaddr);
			if (curaddr->dstaddr)
				free(curaddr->dstaddr);
			free(curaddr);
		}

		/*
		 * Free the name string.
		 */
		free(curdev->name);

		/*
		 * Free the description string, if any.
		 */
		if (curdev->description != NULL)
			free(curdev->description);

		/*
		 * Free the interface.
		 */
		free(curdev);
	}
}

/*
 * Return the name of a network interface attached to the system, or NULL
 * if none can be found.  The interface must be configured up; the
 * lowest unit number is preferred; loopback is ignored.
 */
char *
pcap_lookupdev(errbuf)
	register char *errbuf;
{
	pcap_if_t *alldevs;
/* for old BSD systems, including bsdi3 */
#ifndef IF_NAMESIZE
#define IF_NAMESIZE IFNAMSIZ
#endif
	static char device[IF_NAMESIZE + 1];
	char *ret;

	if (pcap_findalldevs(&alldevs, errbuf) == -1)
		return (NULL);
	
	if (alldevs == NULL || alldevs->is_loopback) {
		/*
		 * There are no devices on the list, or the first device
		 * on the list is a loopback device, which means there
		 * are no non-loopback devices on the list.  This means
		 * we can't return any device.
		 *
		 * XXX - why not return a loopback device?  If we can't
		 * capture on it, it won't be on the list, and if it's
		 * on the list, there aren't any non-loopback devices,
		 * so why not just supply it as the default device?
		 */
		(void)strlcpy(errbuf, "no suitable device found",
		    PCAP_ERRBUF_SIZE);
		ret = NULL;
	} else {
		/*
		 * Return the name of the first device on the list.
		 */
		(void)strlcpy(device, alldevs->name, sizeof(device));
		ret = device;
	}

	pcap_freealldevs(alldevs);
	return (ret);
}

int
pcap_lookupnet(device, netp, maskp, errbuf)
	register char *device;
	register bpf_u_int32 *netp, *maskp;
	register char *errbuf;
{
	register int fd;
	register struct sockaddr_in *sin;
	struct ifreq ifr;

	/* 
	 * The pseudo-device "any" listens on all interfaces and therefore
	 * has the network address and -mask "0.0.0.0" therefore catching
	 * all traffic. Using NULL for the interface is the same as "any".
	 */
	if (!device || strcmp(device, "any") == 0) {
		*netp = *maskp = 0;
		return 0;
	}

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		(void)snprintf(errbuf, PCAP_ERRBUF_SIZE, "socket: %s",
		    pcap_strerror(errno));
		return (-1);
	}
	memset(&ifr, 0, sizeof(ifr));
#ifdef linux
	/* XXX Work around Linux kernel bug */
	ifr.ifr_addr.sa_family = AF_INET;
#endif
	(void)strncpy(ifr.ifr_name, device, sizeof(ifr.ifr_name));
	if (ioctl(fd, SIOCGIFADDR, (char *)&ifr) < 0) {
		if (errno == EADDRNOTAVAIL) {
			(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
			    "%s: no IPv4 address assigned", device);
		} else {
			(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
			    "SIOCGIFADDR: %s: %s",
			    device, pcap_strerror(errno));
		}
		(void)close(fd);
		return (-1);
	}
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	*netp = sin->sin_addr.s_addr;
	if (ioctl(fd, SIOCGIFNETMASK, (char *)&ifr) < 0) {
		(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
		    "SIOCGIFNETMASK: %s: %s", device, pcap_strerror(errno));
		(void)close(fd);
		return (-1);
	}
	(void)close(fd);
	*maskp = sin->sin_addr.s_addr;
	if (*maskp == 0) {
		if (IN_CLASSA(*netp))
			*maskp = IN_CLASSA_NET;
		else if (IN_CLASSB(*netp))
			*maskp = IN_CLASSB_NET;
		else if (IN_CLASSC(*netp))
			*maskp = IN_CLASSC_NET;
		else {
			(void)snprintf(errbuf, PCAP_ERRBUF_SIZE,
			    "inet class for 0x%x unknown", *netp);
			return (-1);
		}
	}
	*netp &= *maskp;
	return (0);
}
