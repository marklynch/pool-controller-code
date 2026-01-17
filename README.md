# Astral Pool Controller

Code to listen on and control an Astral Connect 10 pool controller.

This has been created by listening to the communications on the control bus, and decoding the instructions by trial and error.

## Output
To see the output, either monitor the device using the ESP monitor - or connect to the port exposed on the wifi network.

Example on a mac using nc (netcat)
```
% nc 192.168.0.222 7373
Connected to ESP32-C6 pool bus bridge.
UART bytes will be shown here in hex.
Bytes you send will be forwarded to the bus.

00
02 00 50 FF FF 80 00 FD 0F DC 19 0E 01 28 03
00
```


## Initial Provisioning:

If the LED is purple then connect to the POOL_XXXXXX wifi access point on your phone
In your phone browser navigate to http://192.168.4.1
Here you can choose the wifi network and enter the password.
This will save the details to the NVRam.

Note - if the wrong password is entered - it will try to connect for about 30 seconds and then reset to access point mode and you can start again.

Note 2: you need to clear the NVRam to redo this flow via "Erase Flash Memory from device"

## Visual Feedback Flow:
First Boot (No WiFi):
* Blue solid (startup)
* Purple solid (unconfigured detected)
* Purple solid (provisioning mode active)
* Connect to AP → Configure → Device restarts

Subsequent Boots (With WiFi):
* Blue solid (startup)
* Yellow solid (connected & got IP)


## From ESP IDF template

(See the README.md file in the upper level 'examples' directory for more information about examples.)

This is the simplest buildable example. The example is used by command `idf.py create-project`
that copies the project to user specified path and set it's name. For more information follow the [docs page](https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html#start-a-new-project)



## How to use example
We encourage the users to use the example as a template for the new projects.
A recommended way is to follow the instructions on a [docs page](https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html#start-a-new-project).

## Example folder contents

The project **sample_project** contains one source file in C language [main.c](main/main.c). The file is located in folder [main](main).

ESP-IDF projects are built using CMake. The project build configuration is contained in `CMakeLists.txt`
files that provide set of directives and instructions describing the project's source files and targets
(executable, library, or both). 

Below is short explanation of remaining files in the project folder.

```
├── CMakeLists.txt
├── main
│   ├── CMakeLists.txt
│   └── main.c
└── README.md                  This is the file you are currently reading
```
Additionally, the sample project contains Makefile and component.mk files, used for the legacy Make based build system. 
They are not used or needed when building with CMake and idf.py.
