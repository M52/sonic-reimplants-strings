# Sonic Reimplants: Symphonic String Collection

FastSampler instruments for the Sonic Implants Symphonic String Collection (GigaStudio, 2002). This package contains the instruments and an installer. The package does not contain audio. The installer makes the sample banks from your copy of the original .gig files.

## Requirements

- [FastSampler](https://github.com/M52/fastsampler-downloads/releases)
- The 57 .gig files of the Sonic Implants Symphonic String Collection (approximately 14 GB)
- 10 GB of free disk space

## Install

1. Download the zip file for your platform from [Releases](https://github.com/M52/sonic-reimplants-strings/releases).
2. Extract the zip file. Do not move the folders in the `Sonic Reimplants` folder.
3. Start the installer. The steps for each platform are below.
4. Click **Select the folder...**.
5. Select the folder that contains the .gig files. You can also select one .gig file. The installer then finds the other .gig files.
6. Click **Build 97 banks**.
7. Wait until the installer shows **All 97 banks are ready**.
8. In FastSampler, load an instrument from the `Sonic Reimplants/Instruments` folder.

The installer writes the banks to the `Sonic Reimplants/Samples` folder. The instruments find the banks there.

### Windows

1. Double-click `Installer\sr_install.exe`.
2. If Windows shows "Windows protected your PC", click **More info**. Then click **Run anyway**.

### macOS

Apple has not notarized the installer. Thus macOS does not start it. Do these steps one time:

1. Open Terminal.
2. Type `xattr -dr com.apple.quarantine` and a space.
3. Drag the `Sonic Reimplants` folder into the Terminal window.
4. Press Return.
5. Double-click `Installer/sr_install`.

### Linux

The installer needs X11 or XWayland, libX11 and libXext. Most desktop systems have them.

```
cd "Sonic Reimplants/Installer"
./sr_install
```

## Command line

With arguments, the installer does not open a window.

```
sr_install --gig <folder or .gig file> [options]
```

| Argument | Function |
|---|---|
| `--gig <folder or .gig file>` | The folder of the collection, or one .gig file of the collection. The installer also searches the subfolders. You can use `--gig` more than one time. |
| `--out <folder>` | Write the banks to this folder. Default: `Sonic Reimplants/Samples`. |
| `--only <text>` | Build only the banks that have this text in their name. Example: `--only Cellos`. |
| `--threads <n>` | Build this number of banks at the same time. |
| `--force` | Build the banks again that are already in the output folder. |
| `--verify` | Do not build banks. Compare the banks in the output folder with the recipes. |
| `--instruments <folder>` | The instruments folder. After a build, the installer writes the check values of your banks into the Runs instruments in this folder. Default: `Sonic Reimplants/Instruments`. |
| `--recipes <folder>` | The recipes folder. Default: `Installer/recipes`. |
| `--yes` | Do not ask before the build starts. |

Example:

```
sr_install --gig "D:\Libraries\Sonic Implants" --only Cellos --yes
```

## Different editions of the .gig files

If a .gig file has a different size, the installer finds each sample by its name. The installer compares each sample with the recipe. If a sample is different, the installer does not build that bank.

## Data in the banks

Each bank contains an encrypted record of who built it: your account name, your display name, the date and the name of the installer. Only the FastSampler maintainer can read the record. The installer does not send data.

## Build from source

- Windows: `Installer\build.bat`. You need Visual Studio 2019 or later with the C++ tools.
- Linux: `make -C Installer`. You need `build-essential`, `libx11-dev` and `libxext-dev`.
- macOS: `make -C Installer ARCH="-arch arm64 -arch x86_64"`. You need the Xcode command line tools.

## Licence

MIT. Refer to [LICENSE](LICENSE). The installer contains WavPack (BSD), Monocypher (BSD-2-Clause or CC0), Dear ImGui (MIT) and imgui_sw (public domain). Their licences are in `Installer/licenses` in the zip file and in `Installer/3rdparty` in the source.
