#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/clib.h>

#include "m3u.h"

#define printf sceClibPrintf

int str_starts_with(const char *a, const char *b)
{
   return strncmp(a, b, strlen(b)) == 0;
}

void remove_trailing_crlf(char *a)
{
    int length = strlen(a);
    if (length <= 0) {
        return;
    }

    while (length > 0 && (a[length-1] == '\r' || a[length-1] == '\n')) {
        a[length-1] = '\0';
        length--;
    }
}

int m3u_parse(const char *filepath, struct m3u_file **m3ufile_p)
{
    *m3ufile_p = malloc(sizeof(struct m3u_file));
    struct m3u_file *m3ufile = *m3ufile_p;
    if (!m3ufile) {
        printf("Could not allocate memory for m3u_file\n");
        return -1;
    }

    // Init default values
    m3ufile->filepath = filepath;
    m3ufile->playlist_name = NULL;
    m3ufile->nb_entries = 0;
    m3ufile->first_entry = NULL;

    FILE *fp = NULL;

    // Read the m3u file and allocate entries
    fp = fopen(filepath, "r");
    if (!fp) {
        printf("Could not open file %s\n", filepath);
        return -1;
    }

    char buffer[1024] = {0};
    char *title = NULL;
    while (fgets(buffer, 1024, fp)) {
        remove_trailing_crlf(buffer);
        int length = strlen(buffer);
        if (str_starts_with(buffer, "#")) {
            // We have a special metadata
            if (str_starts_with(buffer, "#PLAYLIST:")) {
                if (!m3ufile->playlist_name) {
                    m3ufile->playlist_name = malloc(length - 10);
                    if (!m3ufile->playlist_name) {
                        printf("Cannot allocate playlist name\n");
                        return -1;
                    }
                    strcpy(m3ufile->playlist_name, buffer + 8);
                }
            } else if (str_starts_with(buffer, "#EXTINF:")) {
                int length_until_title = strcspn(buffer, ",") + 1;
                if (length_until_title > 1 && length_until_title < length) {
                    if (title)
                        free(title);
                    
                    title = malloc(length - length_until_title + 1);
                    strncpy(title, buffer + length_until_title, length - length_until_title + 1);
                }
            }
        } else {
            // We have an URL
            struct m3u_entry *entry = malloc(sizeof(struct m3u_entry));
            if (!entry) {
                printf("Cannot allocate m3u_entry %i bytes\n", sizeof(struct m3u_entry));
                return -1;
            }
            
            entry->previous = NULL;
            entry->next = NULL;
            entry->url = malloc(length + 1);
            if (!entry->url) {
                printf("Cannot allocate entry url %i bytes\n", length);
                return -1;
            }
            strncpy(entry->url, buffer, length + 1);
            entry->logo_url = NULL;
            entry->title = NULL;

            if (title) {
                entry->title = title;
                title = NULL;
            }

            if (!m3ufile->first_entry) {
                m3ufile->first_entry = entry;
            } else {
                // Add entry at the end of chain
                struct m3u_entry *entry_ptr = m3ufile->first_entry;
                while (entry_ptr->next) {
                    entry_ptr = entry_ptr->next;
                }

                entry_ptr->next = entry;
                entry->previous = entry_ptr;
            }
        }
    }

    fclose(fp);
}

void m3u_entry_free(struct m3u_entry *m3uentry) {
    if (!m3uentry) {
        return;
    }

    if (m3uentry->url)
        free(m3uentry->url);

    if (m3uentry->logo_url)
        free(m3uentry->logo_url);

    if (m3uentry->title)
        free(m3uentry->title);

    free(m3uentry);
}

void m3u_file_free(struct m3u_file *m3ufile) {
    if (!m3ufile) {
        return;
    }

    if (m3ufile->playlist_name)
        free(m3ufile->playlist_name);

    struct m3u_entry *current = m3ufile->first_entry;
    struct m3u_entry *next = NULL;
    while (current) {
        next = current->next;
        m3u_entry_free(current);
        current = next;
    }

    free(m3ufile);
}
