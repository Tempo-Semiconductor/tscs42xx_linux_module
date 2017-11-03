This repo provides the Linux driver for the TSCS42xx Audio CODEC.
The development of this driver was carried out on the Raspberry Pi, 
using TSCS42xx Audio HAT. This repo also contains everything that is
neccessary to get up and running with the TSCS42xx on the Raspberry Pi.

Kernel versions tested:

3.10

4.9

Raspberry Pi Module Build/Install Directions:

1. Install Kernel Headers

$ sudo apt-get install raspberrypi-kernel-headers

2. Build Modules and overlay

$ ./build.sh

3. Install Modules and overlay

$ sudo ./install.sh

4. Open /boot/config.txt

$ sudo vi /boot/config.txt

5. At the bottom of the file you will see the following:

dtparam=audio=on

6. Comment it out using # so it appears as the following:

#dtparam=audio=on

7. Enable the tempo sound card by adding the following:

dtoverlay=rpi-tscs42xx-overlay

8. Save and exit vi and reboot
