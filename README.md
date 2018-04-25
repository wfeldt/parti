# parti - Partition Information

## About

`parti` shows partition and file system details for

* [Master Boot Record (MBR) Partition Table][mbr]
* [GUID Partition Table (GPT)][gpt]
* [Apple Partition Map][apm]
* [El Torito Bootable CD/DVD][eltorito]
* [FAT file system BPB][fat]

[mbr]: https://en.wikipedia.org/wiki/Master_boot_record
[gpt]: https://en.wikipedia.org/wiki/GUID_Partition_Table
[apm]: https://en.wikipedia.org/wiki/Apple_Partition_Map
[eltorito]: https://en.wikipedia.org/wiki/El_Torito_%28CD-ROM_standard%29
[fat]: https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system#BPB

It shows the complete information and not only the interpreted data (unlike
partitioning tools like `fdisk` or `parted`).

So it can be used to verify the data your favorite partitioning tool has
actually written.

## openSUSE Development

The package is automatically submitted from the `master` branch to
[system:install:head](https://build.opensuse.org/package/show/system:install:head/parti)
OBS project. From that place it is forwarded to
[openSUSE Factory](https://build.opensuse.org/project/show/openSUSE:Factory).

You can find more information about this workflow in the [linuxrc-devtools
documentation](https://github.com/openSUSE/linuxrc-devtools#opensuse-development).

## Example

Here's what `parti` says about the
[openSUSE-Tumbleweed installation ISO](http://download.opensuse.org/tumbleweed/iso/openSUSE-Tumbleweed-DVD-i586-Current.iso):

```
cd.iso: 9023488 sectors
- - - - - - - - - - - - - - - -
file system:
  fs "iso9660", label "openSUSE-Tumbleweed-DVD-x86_6411", uuid "2018-04-25-14-35-10-00"
- - - - - - - - - - - - - - - -
mbr id: 0x7322d96d
  sector size: 512
  isolinux.bin: 22924, "/boot/x86_64/loader/isolinux.bin"
  mbr partition table (chs 4406/64/32):
  1    3648 - 11459 (size 7812), chs 1/50/1 - 5/38/4
       type 0xef (efi)
       fs "vfat", uuid "732E-5C88", "/boot/x86_64/efi"
       fat12:
         sector size: 512
         bpb[62], oem "mkfs.fat", media 0xf8, drive 0xff, hs 4/63
         vol id 0x732e5c88, label "NO NAME", fs type "FAT12"
         fs size 7812, hidden 0, data start 45
         cluster size 4, clusters 1941
         fats 2, fat size 6, fat start 1
         root entries 512, root size 32, root start 13
  2  * 11460 - 9023487 (size 9012028), chs 5/38/5 - 1023/63/32
       type 0x17 (ntfs hidden)
       fs "iso9660", label "openSUSE-Tumbleweed-DVD-x86_6411", uuid "2018-04-25-14-34-34-00"
- - - - - - - - - - - - - - - -
el torito:
  sector size: 512
  boot catalog: 80
  0    type 0x01 (validation entry)
       platform id 0x00, crc 0x0000 (ok), magic ok
       manufacturer[15] "SUSE LINUX GmbH"
  1  * type 0x88 (initial/default entry)
       boot type 0 (no emulation)
       load address 0x00000, system type 0x00
       start 22924, size 4, "/boot/x86_64/loader/isolinux.bin"
       selection criteria 0x01 "Legacy (x86_64)"
  2    type 0x91 (last section header)
       platform id 0xef, name[0] ""
       entries 1
  3  * type 0x88 (initial/default entry)
       boot type 0 (no emulation)
       load address 0x00000, system type 0x00
       start 3648, size 7812, "/boot/x86_64/efi"
       selection criteria 0x01 "UEFI (x86_64)"
       fs "vfat", uuid "732E-5C88", "/boot/x86_64/efi"
       fat12:
         sector size: 512
         bpb[62], oem "mkfs.fat", media 0xf8, drive 0xff, hs 4/63
         vol id 0x732e5c88, label "NO NAME", fs type "FAT12"
         fs size 7812, hidden 0, data start 45
         cluster size 4, clusters 1941
         fats 2, fat size 6, fat start 1
         root entries 512, root size 32, root start 13

```

You see 3 sections:

  1. the image has a file system on it (ISO9660)
  2. there's an MBR with 2 partitions
    - partition 1 with a FAT file system (the UEFI system partition)
    - partition 2 with an ISO9660 file system
    - Notes
        - the MBR has a link to `isolinux.bin` - a peculiarity of isolinux used to locate itself when the image is booted as a disk
        - partition 1 is also accessible as `/boot/x86_64/efi` in the ISO9660 file system
    3. an El Torito boot record with 2 bootable entries (marked with a `*`)
    - entry 1 used for leagcy BIOS booting (points to `isolinux.bin`)
    - entry 3 used for UEFI booting (points to a FAT image - this is partition 1, btw)
    - Note
        - block size for output is 512 for consistency

And what does this tell us?

The image is both as disk (has MBR) and DVD (El Torito) bootable, via legacy BIOS and UEFI, because
  - as disk it is legcay BIOS bootable (via special MBR boot code that locates `isolinux.bin` and loads it)
  - as disk it is UEFI bootable as it has an EFI System Partition
  - as DVD it is legacy BIOS bootable as it has a suitable El Torito record
  - as DVD it is UEFI bootable as it has another El Torito record suitable for UEFI
