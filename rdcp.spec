Name:           rdcp
Version:        0.1
Release:        2%{?dist}
Summary:        Remote Data Copy (RDMA file copy program)
License:        GPLv2 or OpenIB.org BSD
URL:            https://github.com/roidayan/rdcp
Source0:        https://github.com/roidayan/rdcp/archive/rdcp-%{version}.tar.gz
BuildRequires:  pkgconfig libibverbs-devel librdmacm-devel libxslt docbook-style-xsl

%description
Remote Data Copy (RDMA file copy program)

rdcp copies files between hosts on an rdma capable network.

This utility can be used to copy large files between hosts and is
substantially faster than using traditional copy utilities that
use the TCP protocol, like rcp, scp, ftp. rdcp uses rdma cm to
establish a connection between two capable hosts and uses rdma
operations to copy a file to a listener host.

rdcp is file based, thus it doesn't require from the user to setup
a complicated environment (e.g. iser, cepth, lustre, etc) which
requires pre-configuration of target and initiators.

%prep
%setup -q

%build
export CFLAGS="%{optflags}"
%{__make} %{?_smp_mflags}

%install
%{__install} -d %{buildroot}%{_bindir}
%{__install} -d %{buildroot}%{_mandir}/man8

%{__install} -p -m 0755 rdcp %{buildroot}%{_bindir}
%{__install} -p -m 0644 rdcp.8 %{buildroot}/%{_mandir}/man8

%files
%doc README.md paper.txt
%license COPYING
%{_bindir}/rdcp
%{_mandir}/man8/rdcp.8*

%changelog
* Tue Dec 01 2015 Roi Dayan <roi dot dayan at gmail.com> - 0.1-2
- first package.
