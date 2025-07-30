FROM ubuntu:latest

ENV NOTVISIBLE "in users profile"

# copy files
WORKDIR /xdocker
RUN ls -al
COPY . .

RUN /bin/bash -x dev.sh

# systemctl status
RUN systemctl enable ssh
RUN systemctl enable nginx
RUN rm -rf /xdocker

CMD ["/lib/systemd/systemd"]
