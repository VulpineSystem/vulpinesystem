# VulpineSystem

**VulpineSystem** is a 64 bit fantasy computer based on the RISC-V architecture. It runs an extended/enhanced version of [**xv6**](https://github.com/VulpineSystem/xv6), a Unix-like operating system, and is intended to be used for learning operating system development skills.

The RISC-V emulation core is based on [semu](https://github.com/jserv/semu).

Screenshot of xv6:

![Screenshot](docs/screenshots/helloworld.png)

## Getting Started

### Building

Simply run `make`. The resulting binary will be saved as `vulpinesystem`.

### Usage

`./vulpinesystem <raw kernel image> [<disk image>]`

The most common use case is passing the [**xv6**](https://github.com/VulpineSystem/xv6) kernel image as the first argument, and the filesystem image as the second argument: `./vulpinesystem kernel.bin fs.img`

## License
This project is licensed under the [MIT license](LICENSE).
