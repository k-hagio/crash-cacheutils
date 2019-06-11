cacheutils
==========

cacheutils is an extension module for [crash utility][1] that lists dentry
caches and dumps page caches associated with a specified file path in a vmcore.
The module allows you to find and see page caches of log/config files in your
vmcore.

**NOTE**: To take full advantage of the module, a vmcore needs to be captured
without excluding page caches by [makedumpfile][2].

Getting Started
---------------

To build the module from the top-level `crash-<version>` directory, enter:

    $ cp <path-to>/cacheutils.c extensions
    $ make extensions

To load the module's commands to a running crash session, enter:

    crash> extend <path-to>/crashutils.so

To show the module's commands, enter:

    crash> extend
    SHARED OBJECT            COMMANDS
    <path-to>/cacheutils.so  ccat cls

Help Pages
----------

```
NAME
  cls - list dentry and inode caches

SYNOPSIS
  cls [-adU] [-n pid|task] abspath...

DESCRIPTION
  This command displays the addresses of dentry, inode and i_mapping,
  and nrpages of a specified absolute path and its subdirs if it exists
  in dentry cache.

    -a  also display negative dentries in the subdirs list.
    -d  display the directory itself only, without its contents.
    -U  do not sort, list dentries in directory order.

  For kernels supporting mount namespaces, the -n option may be used to
  specify a task that has the target namespace:

    -n pid   a process PID.
    -n task  a hexadecimal task_struct pointer.

EXAMPLE
  Display the "/var/log/messages" regular file's information:

    crash> cls /var/log/messages
    DENTRY           INODE            I_MAPPING        NRPAGES   % PATH
    ffff9c0c28fda480 ffff9c0c22c675b8 ffff9c0c22c67728     220 100 /var/log/messages

  The '%' column shows the percentage of cached pages in the file.

  Display the "/var/log" directory and its subdirs information:

    crash> cls /var/log
    DENTRY           INODE            I_MAPPING        NRPAGES   % PATH
    ffff9c0c3eabe300 ffff9c0c3e875b78 ffff9c0c3e875ce8       0   0 ./
    ffff9c0c16a22900 ffff9c0c16ada2f8 ffff9c0c16ada468       0   0 anaconda/
    ffff9c0c37611000 ffff9c0c3759f5b8 ffff9c0c3759f728       0   0 audit/
    ffff9c0c375ccc00 ffff9c0c3761c8b8 ffff9c0c3761ca28       1 100 btmp
    ffff9c0c28fda240 ffff9c0c22c713f8 ffff9c0c22c71568       6 100 cron
    ffff9c0c3eb7f180 ffff9c0bfd402a78 ffff9c0bfd402be8      36   7 dnf.librepo.log
    ...

  Display the "/var/log" directory itself only:

    crash> cls -d /var/log
    DENTRY           INODE            I_MAPPING        NRPAGES   % PATH
    ffff9c0c3eabe300 ffff9c0c3e875b78 ffff9c0c3e875ce8       0   0 /var/log/
```
```
NAME
  ccat - dump page caches

SYNOPSIS
  ccat [-S] [-n pid|task] inode|abspath [outfile]

DESCRIPTION
  This command dumps the page caches of a specified inode or path like
  "cat" command.

       -S  do not fseek() and ftruncate() to outfile in order to
           create a non-sparse file.
    inode  a hexadecimal inode pointer.
  abspath  an absolute path.
  outfile  a file path to be written. If a file already exists there,
           the command fails.

  For kernels supporting mount namespaces, the -n option may be used to
  specify a task that has the target namespace:

    -n pid   a process PID.
    -n task  a hexadecimal task_struct pointer.

EXAMPLE
  Dump the existing page caches of the "/var/log/messages" file:

    crash> ccat /var/log/messages
    Sep 16 03:13:01 host systemd: Started Session 559694 of user root.
    Sep 16 03:13:01 host systemd: Starting Session 559694 of user root.
    Sep 16 03:13:39 host dnsmasq-dhcp[24341]: DHCPREQUEST(virbr0) 192.168
    Sep 16 03:13:39 host dnsmasq-dhcp[24341]: DHCPACK(virbr0) 192.168.122
    ...

  Restore the size and data offset of the "messages" file as well to the
  "messages.sparse" file even if some of its page caches don't exist, so
  it could become sparse:

    crash> ccat /var/log/messages messages.sparse

  Create the non-sparse "messages.non-sparse" file:

    crash> ccat -S /var/log/messages messages.non-sparse

  NOTE: Redirecting to a file will also works, but it can includes crash's
  messages, so specifying an outfile is recommended for restoring a file.
```

Tested Kernels
--------------

- RHEL5 to RHEL8 (x86_64)
- Linux 2.6.16 to 5.1 (x86_64 and i686)

Plans
-----

- ~~support mount namespace~~ (supported on 06/11/2019)
- search directories that have many negative dentries

Related Links
-------------

- [crash utility][1] (https://people.redhat.com/anderson/)
- [makedumpfile][2] (https://sourceforge.net/projects/makedumpfile/)

[1]: https://people.redhat.com/anderson/
[2]: https://sourceforge.net/projects/makedumpfile/

Author
------

- Kazuhito Hagio &lt;k-hagio@ab.jp.nec.com&gt;

