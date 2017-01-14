# ariss-tsmerge-viewer-rpi

The board-specific MPEG2 License Key is required.

## Install

### System Dependencies

sudo apt-get install git build-essential libjpeg8-dev indent libfreetype6-dev ttf-dejavu-core

cd /opt/vc/src/hello_pi && sudo ./rebuild.sh

vim /boot/config.txt # Set 'gpu_mem=256'

### Download Repo

git clone --recursive https://github.com/philcrump/ariss-tsmerge-viewer-rpi.git

cd ariss-tsmerge-viewer-rpi/

### Install OpenVG Development Library

cd openvg/ && make && make library && sudo make install && cd ../

### Compile and Install ARISS Graphic Module

cd graphic/ && make && sudo make install && cd ../

### Compile and Install ARISS Video Module

vim ariss_video.service # Set upstream hostname for connection

make && sudo make install

## Bugs

* Pause on every second
** Video appears to otherwise play faster than normal (player is running too fast?)
