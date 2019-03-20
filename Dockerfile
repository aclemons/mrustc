FROM vbatts/slackware:14.2
RUN slackpkg update 
RUN slackpkg -postinst=on -default_answer=yes -batch=on upgrade-all
RUN slackpkg -postinst=on -default_answer=yes -batch=on install glibc  glibc-i18n glibc-profile glibc-zoneinfo
RUN slackpkg -postinst=on -default_answer=yes -batch=on install gc gmp guile libffi libtool libunistring make attr patch acl attr bzip2 curl cyrus-sasl expat fontconfig freetype gcc gcc-g++ glib2 harfbuzz libICE libSM libX11 libXau libXdmcp libXext libXrender libarchive libffi libidn libpng libssh2 libxcb libxml2 lzo ncurses nettle openldap-client openssl openssl-solibs qt util-linux xz zlib cmake libmpc
