echo "Installing shairport-sync dependencies"
sudo apt-get update
sudo apt-get install -y build-essential git xmltoman autoconf automake libtool libdaemon-dev libpopt-dev libconfig-dev libasound2-dev avahi-daemon libavahi-client-dev libssl-dev libsoxr-dev checkinstall libglib2.0-dev

echo "Configuring shairport-sync"
autoreconf -fi
./configure --sysconfdir=/etc --with-alsa --with-soxr --with-avahi --with-ssl=openssl --with-systemd --with-dbus-interface --with-metadata

echo "Compiling"
make

echo "Creating Deb"
sudo checkinstall
