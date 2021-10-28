#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>

#include "disk.h"
#include "filesystem.h"
#include "util.h"
#include "json.h"

#include "eltorito.h"

#define ISO_MAGIC       "\001CD001\001"
#define ELTORITO_MAGIC  "\000CD001\001EL TORITO SPECIFICATION"

// generate output as if we have 512 byte block size
// set to 0 or 2
#define BLK_FIX		2

void dump_eltorito(disk_t *disk)
{
  int i, j;
  unsigned char buf[disk->block_size = 0x800];
  unsigned char zero[32];
  unsigned catalog;
  eltorito_t *el;
  char *s;
  static char *bt[] = {
    "no emulation", "1.2MB floppy", "1.44MB floppy", "2.88MB floppy", "hard disk", ""
  };

  memset(zero, 0, sizeof zero);

  i = disk_read(disk, buf, 0x10, 1);

  if(i || memcmp(buf, ISO_MAGIC, sizeof ISO_MAGIC - 1)) return;

  i = disk_read(disk, buf, 0x11, 1);

  if(i || memcmp(buf, ELTORITO_MAGIC, sizeof ELTORITO_MAGIC - 1)) {
    return;
  }

  json_object *json_eltorito = json_object_new_object();
  json_object_object_add(disk->json_disk, "eltorito", json_eltorito);

  catalog = le32toh(* (uint32_t *) (buf + 0x47));

  log_info(SEP "\nel torito:\n");

  json_object_object_add(json_eltorito, "block_size", json_object_new_int(disk->block_size >> BLK_FIX));
  log_info("  sector size: %d\n", disk->block_size >> BLK_FIX);

  json_object_object_add(json_eltorito, "catalog_lba", json_object_new_int(catalog << BLK_FIX));
  log_info("  boot catalog: %u\n", catalog << BLK_FIX);

  json_object *json_table = json_object_new_array();
  json_object_object_add(json_eltorito, "catalog", json_table);

  i = disk_read(disk, buf, catalog, 1);

  if(i) return;

  for(i = 0; i < disk->block_size/32; i++) {
    el = (eltorito_t *) (buf + 32 * i);
    if(!memcmp(buf + 32 * i, zero, 32)) continue;

    json_object *json_entry = json_object_new_object();
    json_object_array_add(json_table, json_entry);

    json_object_object_add(json_entry, "index", json_object_new_int64(i));
    json_object_object_add(json_entry, "type_id", json_object_new_int64(el->any.header_id));

    switch(el->any.header_id) {
      case 0x01:
        json_object_object_add(json_entry, "type_name", json_object_new_string("validation entry"));
        log_info("  %-3d  type 0x%02x (validation entry)\n", i, el->validation.header_id);

        uint16_t sum;

        for(sum = j = 0; j < 16; j++) {
          sum += buf[32 * i + 2 * j] + (buf[32 * i + 2 * j + 1] << 8);
        }

        uint16_t crc_value = le16toh(el->validation.crc);

        json_object_object_add(json_entry, "platform_id", json_object_new_int64(el->section.platform_id));
        log_info("       platform id 0x%02x", el->validation.platform_id);

        j = le16toh(el->validation.magic);
        json_object_object_add(json_entry, "magic_ok", json_object_new_boolean(j == 0xaa55));

        log_info(", crc 0x%04x (%s)", crc_value - sum, sum ? "wrong" : "ok");
        log_info(", magic %s\n", j == 0xaa55 ? "ok" : "wrong");

        json_object *json_crc = json_object_new_object();
        json_object_object_add(json_entry, "crc", json_crc);
        json_object_object_add(json_crc, "stored", json_object_new_format("0x%04x", crc_value));
        json_object_object_add(json_crc, "calculated", json_object_new_format("0x%04x", crc_value - sum));
        json_object_object_add(json_crc, "ok", json_object_new_boolean(sum == 0));

        s = cname(el->validation.name, sizeof el->validation.name);
        if(*s) json_object_object_add(json_entry, "manufacturer", json_object_new_string(s));
        log_info("       manufacturer[%d] \"%s\"\n", (int) strlen(s), s);
        break;

      case 0x44:
        json_object_object_add(json_entry, "type_name", json_object_new_string("section entry extension"));
        json_object_object_add(json_entry, "info", json_object_new_int64(el->extension.info));
        log_info("  %-3d  type 0x%02x (section entry extension)\n", i, el->any.header_id);
        log_info("       info 0x%02x\n", el->extension.info);
        break;

      case 0x00:
      case 0x88:
        json_object_object_add(json_entry, "type_name", json_object_new_string("initial/default entry"));
        json_object_object_add(json_entry, "boot", json_object_new_boolean(el->any.header_id));
        log_info("  %-3d%c type 0x%02x (initial/default entry)\n", i, el->any.header_id ? '*' : ' ', el->any.header_id);
        json_object_object_add(json_entry, "media_id", json_object_new_int64(el->entry.media));
        s = bt[el->entry.media < 5 ? el->entry.media : 5];
        if(*s) json_object_object_add(json_entry, "media_type", json_object_new_string(s));
        log_info("       boot type %d (%s)\n", el->entry.media, s);
        json_object_object_add(json_entry, "load_address", json_object_new_int64(le16toh(el->entry.load_segment) << 4));
        log_info("       load address 0x%05x", le16toh(el->entry.load_segment) << 4);
        json_object_object_add(json_entry, "system_type", json_object_new_int64(el->entry.system));
        log_info(", system type 0x%02x\n", el->entry.system);
        json_object_object_add(json_entry, "first_lba", json_object_new_int64(le32toh(el->entry.start) << BLK_FIX));
        json_object_object_add(json_entry, "size", json_object_new_int64(le16toh(el->entry.size)));
        log_info("       start %d, size %d%s",
          le32toh(el->entry.start) << BLK_FIX,
          le16toh(el->entry.size),
          BLK_FIX ? "" : "/4"
        );
        if((s = iso_block_to_name(disk, le32toh(el->entry.start) << 2))) {
          json_object_object_add(json_entry, "file_name", json_object_new_string(s));
          log_info(", \"%s\"", s);
        }
        json_object_object_add(json_entry, "criteria_type", json_object_new_int64(el->entry.criteria));
        s = cname(el->entry.name, sizeof el->entry.name);
        if(*s) json_object_object_add(json_entry, "criteria_string", json_object_new_string(s));
        log_info("\n       selection criteria 0x%02x \"%s\"\n", el->entry.criteria, s);
        disk->json_current = json_entry;
        unsigned old_block_size = disk->block_size;
        disk->block_size = 512;
        dump_fs(disk, 7, le32toh(el->entry.start) << BLK_FIX);
        disk->block_size = old_block_size;
        disk->json_current = disk->json_disk;
        break;

      case 0x90:
      case 0x91:
        json_object_object_add(json_entry, "type_name",
          json_object_new_string(el->any.header_id == 0x91 ? "last section header" : "section header")
        );
        log_info(
          "  %-3d  type 0x%02x (%ssection header)\n",
          i,
          el->any.header_id,
          el->any.header_id == 0x91 ? "last " : ""
        );
        json_object_object_add(json_entry, "platform_id", json_object_new_int64(el->section.platform_id));
        log_info("       platform id 0x%02x", el->section.platform_id);
        s = cname(el->section.name, sizeof el->section.name);
        json_object_object_add(json_entry, "name", json_object_new_string(s));
        log_info(", name[%d] \"%s\"\n", (int) strlen(s), s);
        json_object_object_add(json_entry, "entries", json_object_new_int64(el->section.entries));
        log_info("       entries %d\n", le16toh(el->section.entries));
        break;

      default:
        log_info("  %-3d  type 0x%02x\n", i, el->any.header_id);
        break;
    }
  }
}
#undef BLK_FIX


