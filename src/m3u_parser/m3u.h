
#include <psp2/types.h>

struct m3u_entry {
    struct m3u_entry *previous;
    struct m3u_entry *next;
    char *url;
    char *logo_url;
    char *title;
};

struct m3u_file {
    char *filepath;
    char *playlist_name;
    int nb_entries;
    struct m3u_entry *first_entry;
};

int m3u_parse(char *filepath, struct m3u_file **m3ufile_p);
void m3u_file_free(struct m3u_file *m3ufile);
