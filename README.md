# rdcp
Remote Data Copy (RDMA file copy program)


rdcp copies files between hosts on an rdma capable network.

This utility can be used to copy large files between hosts and is substantially faster than using other copy utilities that use the tcp protocol like rcp, scp, ftp.
rdcp uses rdma cm to establish a connection between two capable hosts and uses rdma operations to copy a file to a listener host.

It doesn’t require from the user to setup a complicated environment like iscsi/iser/cepth/etc which requires pre-configuration of the target and initiators sides and needs an entire partition.
