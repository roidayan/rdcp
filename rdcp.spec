%define debug_package %{nil}

Name:           rdcp
Version:        0.1
Release:        2%{?dist}
Summary:        Remote Data Copy (RDMA file copy program)
Packager:       Slava Shwartsman <valyushash@gmail.com>, Roi Dayan <roi.dayan@gmail.com>
Group:          System Environment/Daemons
License:        GPLv2
URL:            https://github.com/roidayan/rdcp
Source0:        %{name}-%{version}-%{release}.tgz
BuildRoot:      %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
BuildRequires:  pkgconfig libibverbs-devel librdmacm-devel libxslt
%if %{defined suse_version}
BuildRequires:  docbook-xsl-stylesheets
Requires:       aaa_base
%else
BuildRequires:  docbook-style-xsl
%endif
Requires:       libibverbs librdmacm
ExcludeArch:    s390 s390x

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
%setup -q -n %{name}-%{version}-%{release}


%build
%{__make} %{?_smp_mflags}


%install
%{__rm} -rf %{buildroot}
%{__install} -d %{buildroot}%{_bindir}
%{__install} -d %{buildroot}%{_mandir}/man8

%{__install} -p -m 0755 rdcp %{buildroot}%{_bindir}
%{__install} -p -m 0644 rdcp.8 %{buildroot}/%{_mandir}/man8

%clean
%{__rm} -rf %{buildroot}


%files
%defattr(-, root, root, -)
%doc README.md paper.txt
%{_bindir}/rdcp
%{_mandir}/man8/rdcp.8*
