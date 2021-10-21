#define SEP		"- - - - - - - - - - - - - - - -"

char *cname(void *buf, int len);

unsigned read_byte(void *buf);
unsigned read_word_le(void *buf);
unsigned read_word_be(void *buf);
unsigned read_dword_le(void *buf);
unsigned read_dword_be(void *buf);
uint64_t read_qword_le(void *buf);
uint64_t read_qword_be(void *buf);
