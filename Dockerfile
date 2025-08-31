FROM ubuntu:latest

LABEL maintainer="Zhigang Song <1005411480a@gmail.com>"

RUN ln -s /usr/share/zoneinfo/Asia/Hong_Kong /etc/localtime
RUN DEBIAN_FRONTEND=noninteractive apt-get update -y
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-change-held-packages openssh-server build-essential vim unzip less git man-db
RUN unminimize
RUN mkdir /var/run/sshd
RUN echo 'root:1' | chpasswd
RUN sed 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' -i /etc/ssh/sshd_config
RUN sed 's@session\s*required\s*pam_loginuid.so@session optional pam_loginuid.so@g' -i /etc/pam.d/sshd
RUN echo "export VISIBLE=now" >> /etc/profile

ENV NOTVISIBLE "in users profile"

RUN systemctl enable ssh.service

VOLUME [/root]
WORKDIR /root
EXPOSE 22
ENTRYPOINT ["/sbin/init"]
