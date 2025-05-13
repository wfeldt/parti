# unify-gpt - GPT(s) for multiple block sizes

## About

unify-gpt creates a GPT setup that works for multiple block sizes.

## Motivation

When preparing disk images the block size of the device the image will be deployed on is not always known in advance.

This makes partitioning the disk image a bit tricky.

An MBR partition table has no notion of a block size. This makes the partition layout depend on the actual block
size of the device.

A GPT partition table also does not explicitly state the block size in its data structures but it can be inferred from
the GPT header location. But you still have to know the relevant block size in advance when creating the disk image.

It is, however, technically possible to have GPTs for different block sizes constructed in a way so that they don't intersect
each other. This way you can have GPTs for multiple block sizes on the same disk image.

For example, a GPT for 512 bytes block size and a GPT for 4096 bytes block size.

unify-gpt supports block sizes 512, 1024, 2048, and 4096 allowing for up to 4 simultaneous GPTs on a disk image.

Partitioning tools like fdisk or parted work fine with this setup but might notify you about additional space that could be used
and offer to correct that.

Note that these different GPTs are only synchronized when created. Once you use partitioning tools to change the partition setup,
GPTs for the non-active block sizes will get out-of-sync or be removed entirely.

You can use `unify-gpt --normalize` to go back to a single-GPT setup before using regular partitioning tools.

Note also that unify-gpt does not create an initial GPT. It works only on already GPT-partitioned images.

## Downloads

unify-gpt is part of the parti package.

Packages for openSUSE and SLES are built at the [openSUSE Build Service](https://build.opensuse.org). You can grab

- [official releases](https://software.opensuse.org/package/parti) or

- [latest stable versions](https://software.opensuse.org/download/package?project=home:snwint:ports&package=parti)
  from my [ports](https://build.opensuse.org/package/show/home:snwint:ports/parti) project

## Example

Lets start with a sample disk image with 4 partitions:

```
# fdisk -b 512 -l foo.img
Disk foo.img: 10 GiB, 10737418240 bytes, 20971520 sectors
Units: sectors of 1 * 512 = 512 bytes
Sector size (logical/physical): 512 bytes / 512 bytes
I/O size (minimum/optimal): 512 bytes / 512 bytes
Disklabel type: gpt
Disk identifier: 70B0FD8F-097E-4F94-9216-6EC38B9B5E5F

Device        Start      End Sectors Size Type
foo.img1       2048  4196351 4194304   2G Linux filesystem
foo.img2    4196352  8390655 4194304   2G Linux filesystem
foo.img3    8390656 16779263 8388608   4G Linux filesystem
foo.img4   16779264 20969471 4190208   2G Linux filesystem
```

There is no GPT for block size 4096 initially:

```
# fdisk -b 4096 -l foo.img
GPT PMBR size mismatch (20971519 != 2621439) will be corrected by write.
Disk foo.img: 10 GiB, 10737418240 bytes, 2621440 sectors
Units: sectors of 1 * 4096 = 4096 bytes
Sector size (logical/physical): 4096 bytes / 4096 bytes
I/O size (minimum/optimal): 4096 bytes / 4096 bytes
Disklabel type: dos
Disk identifier: 0x00000000

Device     Boot Start     End Sectors Size Id Type
foo.img1            1 2621439 2621439  10G ee GPT
```

Note that for a wrong block size fdisk reports the Protective MBR of the GPT and notices the PMBR uses
the wrong block size.

Now run unify-gpt on it:

```
# unify-gpt --add --block-size 4096 foo.img 
found gpt_512: 4 partitions
adding gpt_4096
```

And check the result. There is a GPT for block size 512:

```
# fdisk -b 512 -l foo.img
Disk foo.img: 10 GiB, 10737418240 bytes, 20971520 sectors
Units: sectors of 1 * 512 = 512 bytes
Sector size (logical/physical): 512 bytes / 512 bytes
I/O size (minimum/optimal): 512 bytes / 512 bytes
Disklabel type: gpt
Disk identifier: 70B0FD8F-097E-4F94-9216-6EC38B9B5E5F

Device        Start      End Sectors Size Type
foo.img1       2048  4196351 4194304   2G Linux filesystem
foo.img2    4196352  8390655 4194304   2G Linux filesystem
foo.img3    8390656 16779263 8388608   4G Linux filesystem
foo.img4   16779264 20969471 4190208   2G Linux filesystem
```

And a GPT for block size 4096:

```
# fdisk -b 4096 -l foo.img
Disk foo.img: 10 GiB, 10737418240 bytes, 2621440 sectors
Units: sectors of 1 * 4096 = 4096 bytes
Sector size (logical/physical): 4096 bytes / 4096 bytes
I/O size (minimum/optimal): 4096 bytes / 4096 bytes
Disklabel type: gpt
Disk identifier: 70B0FD8F-097E-4F94-9216-6EC38B9B5E5F

Device       Start     End Sectors Size Type
foo.img1       256  524543  524288   2G Linux filesystem
foo.img2    524544 1048831  524288   2G Linux filesystem
foo.img3   1048832 2097407 1048576   4G Linux filesystem
foo.img4   2097408 2621183  523776   2G Linux filesystem
```

Now let's remove the GPT for block size 512:

```
# unify-gpt --normalize --block-size 4096 foo.img 
found gpt_512: 4 partitions
found gpt_4096: 4 partitions
deleting gpt_512
keeping gpt_4096
```

GPT for block size 4096 is still there:

```
# fdisk -b 4096 -l foo.img 
Disk foo.img: 10 GiB, 10737418240 bytes, 2621440 sectors
Units: sectors of 1 * 4096 = 4096 bytes
Sector size (logical/physical): 4096 bytes / 4096 bytes
I/O size (minimum/optimal): 4096 bytes / 4096 bytes
Disklabel type: gpt
Disk identifier: 70B0FD8F-097E-4F94-9216-6EC38B9B5E5F

Device       Start     End Sectors Size Type
foo.img1       256  524543  524288   2G Linux filesystem
foo.img2    524544 1048831  524288   2G Linux filesystem
foo.img3   1048832 2097407 1048576   4G Linux filesystem
foo.img4   2097408 2621183  523776   2G Linux filesystem
```

But GPT for block size 512 is gone. Only the PMBR is reported, again with wrong
block size (as it has been created for block size 4096):

```
# fdisk -b 512 -l foo.img 
GPT PMBR size mismatch (2621439 != 20971519) will be corrected by write.
Disk foo.img: 10 GiB, 10737418240 bytes, 20971520 sectors
Units: sectors of 1 * 512 = 512 bytes
Sector size (logical/physical): 512 bytes / 512 bytes
I/O size (minimum/optimal): 512 bytes / 512 bytes
Disklabel type: dos
Disk identifier: 0x00000000

Device     Boot Start      End  Sectors Size Id Type
foo.img1            1 20971519 20971519  10G ee GPT
```


## openSUSE Development

To build, simply run `make`. Install with `make install`.

Basically every new commit into the master branch of the repository will be auto-submitted
to all current SUSE products. No further action is needed except accepting the pull request.

Submissions are managed by a SUSE internal [jenkins](https://jenkins.io) node in the InstallTools tab.

Each time a new commit is integrated into the master branch of the repository,
a new submit request is created to the openSUSE Build Service. The devel project
is [system:install:head](https://build.opensuse.org/package/show/system:install:head/parti).

`*.changes` and version numbers are auto-generated from git commits, you don't have to worry about this.

The spec file is maintained in the Build Service only. If you need to change it for the `master` branch,
submit to the
[devel project](https://build.opensuse.org/package/show/system:install:head/parti)
in the build service directly.

Development happens exclusively in the `master` branch. The branch is used for all current products.

You can find more information about the changes auto-generation and the
tools used for jenkis submissions in the [linuxrc-devtools
documentation](https://github.com/openSUSE/linuxrc-devtools#opensuse-development).

