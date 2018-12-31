![Tempo Logo](https://github.com/Tempo-Semiconductor/tempo_res/blob/master/tempo.png)

# TSCS42xx Linux Module

This repo provides the Linux driver for the TSCS42xx Audio CODEC.
The development of this driver was carried out on the Raspberry Pi, 
using TSCS42xx Audio HAT. This repo also contains everything that is
neccessary to get up and running with the TSCS42xx on the Raspberry Pi.

#### Raspberry Pi Module Build/Install Directions:

1. Install Kernel Headers

`$ sudo apt-get install raspberrypi-kernel-headers`

2. Clone the repository

    $ git clone https://github.com/Tempo-Semiconductor/tscs42xx_linux_module.git

3. Change directory to the cloned repository 

    $ cd tscs42xx_linux_module

4. Build Modules and overlay

    $ ./build.sh

5. Install Modules and overlay

    $ sudo ./install.sh

6. Open /boot/config.txt

    $ sudo vi /boot/config.txt

7. At the bottom of the file you will see the following:

    dtparam=audio=on

8. Comment it out using # so it appears as the following:

    #dtparam=audio=on

9. Enable the tempo sound card by adding the following:

    dtoverlay=rpi-tscs42xx-overlay

10. Save and exit vi and reboot
