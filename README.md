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

It shows the complete information but mostly in uninterpreted form (unlike
partitioning tools like `fdisk` or `parted`).

So it can be used to verify the data your favorite partitioning tool has
actually written.

## openSUSE Development

The package is automatically submitted from the `master` branch to
[home:snwint](https://build.opensuse.org/package/show/home:snwint/parti)
OBS project. However, unlike other projects, is not submitted automatically to
openSUSE:Factory.

You can find more information about this workflow in the [linuxrc-devtools
documentation](https://github.com/openSUSE/linuxrc-devtools#opensuse-development).
