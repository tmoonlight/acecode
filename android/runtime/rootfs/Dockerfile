# 安卓 App 自带的 Ubuntu 小系统。用 buildx 直接导出文件系统:
#   docker buildx build --platform linux/arm64 --output type=tar,dest=rootfs.tar .
# 包只装 coding agent 日常要用的;用户之后可以自己 apt install。
ARG UBUNTU_IMAGE=ubuntu:24.04
FROM ${UBUNTU_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive

# proot 里只有假 root,apt 降权到 _apt 会失败。
RUN printf 'APT::Sandbox::User "root";\n' > /etc/apt/apt.conf.d/01-acecode-proot \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      git \
      curl \
      less \
      procps \
      openssh-client \
      python3 \
      tzdata \
      xz-utils \
      unzip \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*.deb /var/log/apt /var/log/dpkg.log \
 && mkdir -p /opt/acecode
