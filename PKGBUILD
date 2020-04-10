# Maintainer: Jan de Groot <jgc@archlinux.org>
# Contributor: Tobias Powalowski  <tpowa@archlinux.org>
# Contributor: Thomas Bächler <thomas@archlinux.org>
# Contributor: Alexander Baldeck <alexander@archlinux.org>

pkgname=xf86-input-synaptics
pkgver=1.9.1
pkgrel=1
pkgdesc="MTJ's touch driver"
arch=('x86_64')
license=('MIT')
url="https://xorg.freedesktop.org/"
depends=('libxtst' 'libevdev')
makedepends=('xorg-server-devel' 'X-ABI-XINPUT_VERSION=24.1' 'libxi' 'libx11' 'resourceproto' 'scrnsaverproto')
conflicts=('xorg-server<1.19' 'X-ABI-XINPUT_VERSION<24.1' 'X-ABI-XINPUT_VERSION>=25')
replaces=('synaptics')
provides=('synaptics')
conflicts=('synaptics')
groups=('xorg-drivers')
install=xf86-input-synaptics.install
build() {
  cd ${pkgname}-${pkgver}
  
  ./configure --prefix=/usr
  make
}

package() {
  cd ${pkgname}-${pkgver}
  
  make DESTDIR="${pkgdir}" install
  install -m755 -d "${pkgdir}/usr/share/licenses/${pkgname}"
  install -m644 COPYING "${pkgdir}/usr/share/licenses/${pkgname}/"
}
