# rdcp
Remote Data Copy (RDMA file copy program)


rdcp copies files between hosts on an rdma capable network.

This utility can be used to copy large files between hosts and is substantially faster than using traditional copy utilities that use the TCP protocol, like rcp, scp, ftp.
rdcp uses rdma cm to establish a connection between two capable hosts and uses rdma operations to copy a file to a listener host.

rdcp is file based, thus it doesn’t require from the user to setup a complicated environment (e.g. iser, cepth, lustre, etc) which requires pre-configuration of target and initiators.

# Authors
- Roi Dayan
- Slava Shwartsman
