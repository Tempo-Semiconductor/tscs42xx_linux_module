This repo provides the Linux driver for the TSCS42xx Audio CODEC.
The development of this driver was carried out on the Raspberry Pi, 
using TSCS42xx Audio HAT. This repo also contains everything that is
neccessary to get up and running with the TSCS42xx on the Raspberry Pi.

Kernel versions tested:

3.10

4.9

Directions:

1. Build Modules and overlay

$ ./build.sh

2. Install Modules and overlay

$ sudo ./install.sh

3. Open /boot/config.txt

$ sudo vi /boot/config.txt

4. At the bottom of the file you will see the following:

dtparam=audio=on

5. Comment it out using # so it appears as the following:

#dtparam=audio=on

6. Enable the tempo sound card by adding the following:

dtoverlay=rpi-tscs42xx-overlay

7. Save and exit vi and reboot
