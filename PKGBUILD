pkgname=deadbeef-plugin-stereo-spectrogram-git
pkgver=20250805
pkgrel=1
pkgdesc="Stereo Spectrogram Plugin for the DeaDBeeF audio player (development version)"
url="https://github.com/x00FB/ddb_stereo_spectrogram"
arch=('i686' 'x86_64')
license='GPL2'
depends=('deadbeef' 'fftw')
makedepends=('git')

_gitname=ddb_stereo_spectrogram
_gitroot=https://github.com/x00FB/${_gitname}

build() {
  cd $srcdir
  msg "Connecting to GIT server..."
  rm -rf $srcdir/$_gitname-build

  if [ -d $_gitname ]; then
    cd $_gitname
    git pull origin master
  else
    git clone $_gitroot
  fi

  msg "GIT checkout done or server timeout"
  msg "Starting make..."

  cd $srcdir
  cp -r $_gitname $_gitname-build

  cd $_gitname-build

  touch AUTHORS
  touch ChangeLog

  make
}

package() {
  install -D -v -c $srcdir/$_gitname-build/gtk2/ddb_vis_stereo_spectrogram_GTK2.so $pkgdir/usr/lib/deadbeef/ddb_vis_stereo_spectrogram_GTK2.so
  install -D -v -c $srcdir/$_gitname-build/gtk3/ddb_vis_stereo_spectrogram_GTK3.so $pkgdir/usr/lib/deadbeef/ddb_vis_stereo_spectrogram_GTK3.so
}
