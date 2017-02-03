/*
 *  XenLoop -- A High Performance Inter-VM Network Loopback 
 *
 *  Installation and Usage instructions
 *
 *  Authors: 
 *  	Jian Wang - Binghamton University (jianwang@cs.binghamton.edu)
 *  	Kartik Gopalan - Binghamton University (kartik@cs.binghamton.edu)
 *
 *  Copyright (C) 2007-2009 Kartik Gopalan, Jian Wang
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


			===================
			XENLOOP Version 2.0
			===================
		http://osnet.cs.binghamton.edu/projects/xenloop.html

Contents
========
What is XenLoop?
Where to Get XenLoop?
XenLoop Binary Components
Compatibility
Building XenLoop
Installing XenLoop
Using XenLoop
Testing XenLoop
Some Adjustable Parameters
Some Other Useful Components of XenLoop
Feedback Welcome

=====================================

What is XenLoop?
================

XenLoop is a transparent inter-VM network loopback 
mechanism to communicate between Linux guest VMs 
that are co-resident on the same physical machine. 
XenLoop uses shared memory to bypass the standard 
network data path via netback/netfront and Domain 0
and their associated performance overheads.
There is no need to modify the code in Linux guests
or the hypervisor or the networking applications
in order to use XenLoop.  XenLoop also transparently 
adapts to guest migration without disrupting any
ongoing network communication.

This file contains the installation and usage instructions 
for XenLoop. More detailed documentation regarding design 
and implementation of XenLoop is available in our 
technical report at 
http://osnet.cs.binghamton.edu/projects/xenloop.html


Where to Get XenLoop?
=====================
The latest release of XenLoop is available at
http://osnet.cs.binghamton.edu/projects/xenloop.html


XenLoop Binary Components
=========================
XenLoop is designed as two kernel modules

- xenloop.ko 
	This module is meant to be inserted in each Linux guest
	in which you want to enable the inter-VM loopback 
	functionality.

- discovery.ko
	This module is to be inserted in Domain 0, and helps in 
	discovering and announcing co-resident Linux guests that 
	have an active XenLoop module.

Xenloop does not require any changes to either
the Linux guest code or the Xen hypervisor code.


Compatibility
=============

This distribution contains sources that are known to work with
the following:
- paravirtualized Linux 2.6.18 and 2.6.18.8 guests
- Xen 3.2. 

All the Linux guests using XenLoop need have "netfilter" 
enabled in kernel. 
	http://www.netfilter.org/
This is standard in most linux kernel distributions, but 
you may want to check.

We have not yet tested XenLoop with later versions of Xen 
and Linux, nor have we tested it with HVMs. 
Also, interaction of XenLoop with any firewalls 
(such as iptables) has not been tested.
We welcome feedback on any porting related issues.


Building XenLoop
================

To compile XenLoop, simply execute 

$ make 

in the source directory. This should generate the two 
modules, xenloop.ko and discovery.ko. 

You will need to compile the modules against your specific 
Linux kernel versions of both the Linux guest and Domain 0.


Installing XenLoop
==================

Transfer discovery.ko to Domain 0 and do the following:

$ insmod discovery.ko

In each Linux guest where you wish to run xenloop,
transfer xenloop.ko and do the following:

$ insmod xenloop.ko

The order of above operations does not matter.
What matters is that all modules be installed 
for everything to work.

Alternatively, you can set up your system to load
the modules at boot time, depending upon your system 
configuration. Only make sure that the modules 
are loaded **AFTER** the network interfaces are 
brought up. This is because XenLoop uses the
network interface to communicate out-of-band 
control messages.


Using XenLoop
=============

Once the modules are installed as described above,
you don't have to do anything special to use xenloop,
other than to run your unmodified networking 
applications -- TCP/UDP/ICMP/raw sockets etc.

Currently XenLoop forwards only IP traffic via 
inter-VM channel. All other traffic passes
via the standard netfront/netback interface.
This could likely be extended to other protocols 
with minor coding effort.

XenLoop modules automatically discover each others'
presence or absence in co-resident guests.
In the former case, they will transparently set up
and use the inter-VM loopback channel. In the latter
case, they will transparently use
the netfront/netback interface.

Any normal network communication between guests and 
outside machines remains unaffected.

Running guests can also be migrated without 
interrupting applications using XenLoop channels
and without removing the xenloop and discovery 
modules. XenLoop modules automatically discover
when a co-resident guest migrates away, or a remote
guest becomes co-resident; correspondingly they
tear down or set up inter-VM loopback channels.


Testing XenLoop
===============

To test whether your installed XenLoop is working,
ping between co-resident guests without the modules and
note the RTT. Then insert the modules
and ping again. The RTT with XenLoop 
should be about many times smaller than without 
XenLoop. We observed a factor of 5 to 6 improvement 
in RTT on our dual-core machines.

You can also test XenLoop throughput performance on 
your system using standard unmodified benchmarks such 
as netperf, lmbench, netpipe-mpich etc.


Some Adjustable Parameters in The Code
======================================

MAX_MAC_NUM:
	If you want to install Xenloop on more than 10 
	guests in one physical machine, you can 
	adjust the "MAX_MAC_NUM" parameter accordingly 
	in discovery.h and main.h.

XENLOOP_ENTRY_ORDER:
	(This parameter is rendered less useful with 
	 XenLoop release 2.0 onwards)
	"XENLOOP_ENTRY_ORDER" in xenloop.h determines 
	the number of FIFO entries in each direction. 

	Number of entries = 2 ^ XENLOOP_ENTRY_ORDER

MAX_FIFO_PAGES
	"MAX_FIFO_PAGES" in xenfifo.h  defines the maximum 
	number of shared memory pages you could can use for 
	each FIFO. Again this can be changed, but you may 
	hit a hypervisor-imposed limit at some point.

Feel free to browse the code for other parameters you 
may want to tweak, such as periodic discovery 
announcement interval from Domain 0, timeout intervals
for control message replies etc.

Some Other Useful Components of XenLoop
========================================
There are a few components in XenLoop that may be
useful in their own right. 

- xenfifo.c and xenfifo.h provide a unidirectional
  FIFO functionality using inter-domain shared memory.
  They implement a lockless producer-consumer interaction,
  assuming that one guest is the producer and another 
  is consumer. There is nothing to enforce these roles 
  though and both guests can act as both producers 
  and consumers. If there can be multiple producer
  or consumer threads in a guest, you may want to use 
  your own producer-local and consumer-local locks, like 
  we do in XenLoop. The FIFO manipulation primitives, 
  we hope, are self-contained enough to be used in 
  other context with little or no modification.

- The discovery module could also be used, with 
  minor modifications, in other contexts where one
  wishes to transparently discover other co-resident 
  domains.

Feedback Welcome
================

Please email the authors at 
	kartik@cs.binghamton.edu 
	jianwang@cs.binghamton.edu

We are particularly eager to hear about usage feedback such as 
	- What applications did you use XenLoop for?
	- What kind of performance improvements did you obtain (or not obtain)?
	- What problems did you face?
	- What improvements can be made?

If you use XenLoop in a product or to publish any 
research results, please do let us know as courtesy so that 
we can link to your work from our project webpage.

We also welcome any constructive suggestions, bug reports, 
bug fixes, feature enhancements, and ports. 

