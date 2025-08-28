#!/bin/bash

# 软件安装
RUN apt-key adv --keyserver hkp://keyserver.ubuntu.com:80 --recv-keys BB06368F66B778F9

RUN DEBIAN_FRONTEND=noninteractive apt-get update -y &&
    apt-get upgrade -y &&
    apt-get install -y systemd openssh-server wget vim git \
        psmisc net-tools unzip less nginx-full rsync inotify-tools \
        build-essential flex bison automake libffi-dev jq \
        thrift-compiler man manpages-posix \
        python3 python3-requests

# 初始化
echo 'set nu' > ~/.vimrc
echo "export VISIBLE=now" >> /etc/profile

sed '/EnvironmentFile/iRuntimeDirectory=sshd' -i /lib/systemd/system/ssh.service
sed 's@session\s*required\s*pam_loginuid.so@session optional pam_loginuid.so@g' -i /etc/pam.d/sshd
sed 's/#\?Port 22/Port 6022/;
     s/PermitRootLogin without-password/PermitRootLogin yes/;
     s/#AuthorizedKeysFile/AuthorizedKeysFile/;
     s/#PasswordAuthentication/PasswordAuthentication/' -i /etc/ssh/sshd_config

# nginx proxy server
sed '/^\s\+# server_tokens off/aclient_max_body_size 200M;' -i /etc/nginx/nginx.conf

# password setting
echo 'root:1111' | chpasswd
