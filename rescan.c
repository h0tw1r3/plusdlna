/* Modified inotify.c by Shrimpkin@SourceForge.net 20150519 
 *
 * MiniDLNA media server
 * Copyright (C) 2008-2010  Justin Maggard
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <errno.h>
#include <sys/stat.h>
#include "libav.h"

#include "upnpglobalvars.h"
#include "utils.h"
#include "sql.h"
#include "scanner.h"
#include "metadata.h"
#include "albumart.h"
#include "playlist.h"
#include "log.h"

int
rescan_remove_file(const char * path)
{
	char sql[128];
	char art_cache[PATH_MAX];
	char *id;
	char *ptr;
	char **result;
	int64_t detailID;
	int rows, playlist;

	if( is_caption(path) )
	{
		return sql_exec(db, "DELETE from CAPTIONS where PATH = '%q'", path);
	}
	/* Invalidate the scanner cache so we don't insert files into non-existent containers */
	valid_cache = 0;
	playlist = is_playlist(path);
	id = sql_get_text_field(db, "SELECT ID from %s where PATH = '%q'", playlist?"PLAYLISTS":"DETAILS", path);
	if( !id )
		return 1;
	detailID = strtoll(id, NULL, 10);
	sqlite3_free(id);
	if( playlist )
	{
		sql_exec(db, "DELETE from PLAYLISTS where ID = %lld", detailID);
		sql_exec(db, "DELETE from DETAILS where ID ="
		             " (SELECT DETAIL_ID from OBJECTS where OBJECT_ID = '%s$%llX')",
		         MUSIC_PLIST_ID, detailID);
		sql_exec(db, "DELETE from OBJECTS where OBJECT_ID = '%s$%llX' or PARENT_ID = '%s$%llX'",
		         MUSIC_PLIST_ID, detailID, MUSIC_PLIST_ID, detailID);
	}
	else
	{
		/* Delete the parent containers if we are about to empty them. */
		snprintf(sql, sizeof(sql), "SELECT PARENT_ID from OBJECTS where DETAIL_ID = %lld"
		                           " and PARENT_ID not like '64$%%'",
		                           (long long int)detailID);
		if( (sql_get_table(db, sql, &result, &rows, NULL) == SQLITE_OK) )
		{
			int i, children;
			for( i = 1; i <= rows; i++ )
			{
				/* If it's a playlist item, adjust the item count of the playlist */
				if( strncmp(result[i], MUSIC_PLIST_ID, strlen(MUSIC_PLIST_ID)) == 0 )
				{
					sql_exec(db, "UPDATE PLAYLISTS set FOUND = (FOUND-1) where ID = %d",
					         atoi(strrchr(result[i], '$') + 1));
				}

				children = sql_get_int_field(db, "SELECT count(*) from OBJECTS where PARENT_ID = '%s'", result[i]);
				if( children < 0 )
					continue;
				if( children < 2 )
				{
					sql_exec(db, "DELETE from OBJECTS where OBJECT_ID = '%s'", result[i]);

					ptr = strrchr(result[i], '$');
					if( ptr )
						*ptr = '\0';
					if( sql_get_int_field(db, "SELECT count(*) from OBJECTS where PARENT_ID = '%s'", result[i]) == 0 )
					{
						sql_exec(db, "DELETE from OBJECTS where OBJECT_ID = '%s'", result[i]);
					}
				}
			}
			sqlite3_free_table(result);
		}
		/* Now delete the actual objects */
		sql_exec(db, "DELETE from DETAILS where ID = %lld", detailID);
		sql_exec(db, "DELETE from OBJECTS where DETAIL_ID = %lld", detailID);
	}
	snprintf(art_cache, sizeof(art_cache), "%s/art_cache%s", db_path, path);
	remove(art_cache);

	return 0;
}

int
rescan_insert_file(char * name, const char * path)
{
	int len;
	char * last_dir;
	char * path_buf;
	char * base_name;
	char * base_copy;
	char * parent_buf = NULL;
	char * id = NULL;
	int depth = 1;
	int ts;
	media_types types = ALL_MEDIA;
	struct media_dir_s * media_path = media_dirs;
	struct stat st;

	/* Is it cover art for another file? */
	if( is_image(path) )
		update_if_album_art(path);
	else if( is_caption(path) )
		check_for_captions(path, 0);

	/* Check if we're supposed to be scanning for this file type in this directory */
	while( media_path )
	{
		if( strncmp(path, media_path->path, strlen(media_path->path)) == 0 )
		{
			types = media_path->types;
			break;
		}
		media_path = media_path->next;
	}
	switch( types )
	{
		case ALL_MEDIA:
			if( !is_image(path) &&
			    !is_audio(path) &&
			    !is_video(path) &&
			    !is_playlist(path) )
				return -1;
			break;
		case TYPE_AUDIO:
			if( !is_audio(path) &&
			    !is_playlist(path) )
				return -1;
			break;
		case TYPE_AUDIO|TYPE_VIDEO:
			if( !is_audio(path) &&
			    !is_video(path) &&
			    !is_playlist(path) )
			break;
		case TYPE_AUDIO|TYPE_IMAGES:
			if( !is_image(path) &&
			    !is_audio(path) &&
			    !is_playlist(path) )
				return -1;
			break;
		case TYPE_VIDEO:
			if( !is_video(path) )
				return -1;
			break;
		case TYPE_VIDEO|TYPE_IMAGES:
			if( !is_image(path) &&
			    !is_video(path) )
				return -1;
			break;
		case TYPE_IMAGES:
			if( !is_image(path) )
				return -1;
			break;
	              default:
			return -1;
			break;
	}
	
	/* If it's already in the database and hasn't been modified, skip it. */
	if( stat(path, &st) != 0 )
		return -1;

	ts = sql_get_int_field(db, "SELECT TIMESTAMP from DETAILS where PATH = '%q'", path);
	if( !ts && is_playlist(path) && (sql_get_int_field(db, "SELECT ID from PLAYLISTS where PATH = '%q'", path) > 0) )
	{
		DPRINTF(E_DEBUG, L_SCANNER, "Re-reading modified playlist (%s).\n", path);
		rescan_remove_file(path);
	}
	else if( ts != st.st_mtime )
	{
	  if (!ts)
	    DPRINTF(E_DEBUG, L_SCANNER, "Adding %s\n", path);
	  else
	    DPRINTF(E_DEBUG, L_SCANNER, "%s is %s than the last db entry.\n", path, (ts < st.st_mtime) ? "older" : "newer");
		rescan_remove_file(path);
	}
	else if (ts == st.st_mtime)
	{
	  //DPRINTF(E_DEBUG, L_SCANNER, "%s already exists\n", path);
	  return 0;
	}

	/* Find the parentID.  If it's not found, create all necessary parents. */
	len = strlen(path)+1;
	if( !(path_buf = malloc(len)) ||
	    !(last_dir = malloc(len)) ||
	    !(base_name = malloc(len)) )
		return -1;
	base_copy = base_name;
	while( depth )
	{
		depth = 0;
		strcpy(path_buf, path);
		parent_buf = dirname(path_buf);

		do
		{
			//DEBUG DPRINTF(E_DEBUG, L_SCANNER, "Checking %s\n", parent_buf);
			id = sql_get_text_field(db, "SELECT OBJECT_ID from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
			                            " where d.PATH = '%q' and REF_ID is NULL", parent_buf);
			if( id )
			{
				if( !depth )
					break;
				DPRINTF(E_DEBUG, L_SCANNER, "Found first known parentID: %s [%s]\n", id, parent_buf);
				/* Insert newly-found directory */
				strcpy(base_name, last_dir);
				base_copy = basename(base_name);
				insert_directory(base_copy, last_dir, BROWSEDIR_ID, id+2, get_next_available_id("OBJECTS", id));
				sqlite3_free(id);
				break;
			}
			depth++;
			strcpy(last_dir, parent_buf);
			parent_buf = dirname(parent_buf);
		}
		while( strcmp(parent_buf, "/") != 0 );

		if( strcmp(parent_buf, "/") == 0 )
		{
			id = sqlite3_mprintf("%s", BROWSEDIR_ID);
			depth = 0;
			break;
		}
		strcpy(path_buf, path);
	}
	free(last_dir);
	free(path_buf);
	free(base_name);

	if( !depth )
	{
		//DEBUG DPRINTF(E_DEBUG, L_SCANNER, "Inserting %s\n", name);
		insert_file(name, path, id+2, get_next_available_id("OBJECTS", id), types);
		sqlite3_free(id);
	}
	return depth;
}

int
rescan_insert_directory(char *name, const char * path)
{
	DIR * ds;
	struct dirent * e;
	char *id, *parent_buf, *esc_name;
	char path_buf[PATH_MAX];
	enum file_types type = TYPE_UNKNOWN;
	media_types dir_types = ALL_MEDIA;
	struct media_dir_s* media_path;
	struct stat st;

	if( access(path, R_OK|X_OK) != 0 )
	{
		DPRINTF(E_WARN, L_SCANNER, "Could not access %s [%s]\n", path, strerror(errno));
		return -1;
	}

	if( sql_get_int_field(db, "SELECT ID from DETAILS where PATH = '%q'", path) > 0 )
	{
		; //DPRINTF(E_DEBUG, L_SCANNER, "%s already exists\n", path);
	}
	else
	{
	 	parent_buf = strdup(path);
		id = sql_get_text_field(db, "SELECT OBJECT_ID from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
		                            " where d.PATH = '%q' and REF_ID is NULL", dirname(parent_buf));
		if( !id )
	  {
			id = sqlite3_mprintf("%s", BROWSEDIR_ID);
		  insert_directory(name, path, BROWSEDIR_ID, id+2, get_next_available_id("OBJECTS", id));
		  sqlite3_free(id);
	  }
		free(parent_buf);
	}

	media_path = media_dirs;
	while( media_path )
	{
		if( strncmp(path, media_path->path, strlen(media_path->path)) == 0 )
		{
			dir_types = media_path->types;
			break;
		}
		media_path = media_path->next;
	}

	ds = opendir(path);
	if( !ds )
	{
		DPRINTF(E_ERROR, L_SCANNER, "opendir failed! [%s]\n", strerror(errno));
		return -1;
	}
	while( (e = readdir(ds)) )
	{
		if( e->d_name[0] == '.' )
			continue;
		esc_name = escape_tag(e->d_name, 1);
		snprintf(path_buf, sizeof(path_buf), "%s/%s", path, e->d_name);
		switch( e->d_type )
		{
			case DT_DIR:
			case DT_REG:
			case DT_LNK:
			case DT_UNKNOWN:
				type = resolve_unknown_type(path_buf, dir_types);
			default:
				break;
		}
		if( type == TYPE_DIR )
		{
			rescan_insert_directory(esc_name, path_buf);
		}
		else if( type == TYPE_FILE )
		{
			if( (stat(path_buf, &st) == 0) && (st.st_blocks<<9 >= st.st_size) )
			{
				rescan_insert_file(esc_name, path_buf);
			}
		}
		free(esc_name);
	}
	closedir(ds);

	return 0;
}

int
rescan_remove_directory(const char * path)
{
	char * sql;
	char **result;
	int64_t detailID = 0;
	int rows, i, ret = 1;

	/* Invalidate the scanner cache so we don't insert files into non-existent containers */
	valid_cache = 0;
	sql = sqlite3_mprintf("SELECT ID from DETAILS where (PATH > '%q/' and PATH <= '%q/%c')"
	                      " or PATH = '%q'", path, path, 0xFF, path);
	if( (sql_get_table(db, sql, &result, &rows, NULL) == SQLITE_OK) )
	{
		if( rows )
		{
			for( i=1; i <= rows; i++ )
			{
				detailID = strtoll(result[i], NULL, 10);
				sql_exec(db, "DELETE from DETAILS where ID = %lld", detailID);
				sql_exec(db, "DELETE from OBJECTS where DETAIL_ID = %lld", detailID);
			}
			ret = 0;
		}
		sqlite3_free_table(result);
	}
	sqlite3_free(sql);
	/* Clean up any album art entries in the deleted directory */
	sql_exec(db, "DELETE from ALBUM_ART where (PATH > '%q/' and PATH <= '%q/%c')", path, path, 0xFF);

	return ret;
}

static int
cb_orphans(void *args, int argc, char **argv, char **azColName)
{
	struct stat file;
	char *path = argv[0], *mime = argv[1];

	/* If we can't stat path, remove it */
	if ( stat (path, &file) != 0)
	{
	  DPRINTF(E_WARN, L_SCANNER, "Removing %s [%s]!\n", path, (mime) ? "file" : "dir");
	  if ( mime )
	  {
	    rescan_remove_file (path);
	  }
	  else
	  {
	    rescan_remove_directory (path);
	  }
	}
	return 0;
}

void
rescan_scanner(void)
{
	struct media_dir_s *media_path;
	char path[MAXPATHLEN], buf[MAXPATHLEN], *esc_name = NULL, *zErrMsg;
	char *sql_files = "SELECT path, mime FROM details WHERE path NOT NULL AND mime IS NOT NULL;";
	char *sql_dir = "SELECT path, mime FROM details WHERE path NOT NULL AND mime IS NULL;";
	int ret;

	DPRINTF(E_WARN, L_SCANNER, "Starting rescan\n");

	/* Find and remove any dead directory links */
	ret = sqlite3_exec(db, sql_dir, cb_orphans, NULL, &zErrMsg);
	if ( ret != SQLITE_OK )
	{
	  DPRINTF(E_MAXDEBUG, L_SCANNER, "SQL error: %s\nBAD SQL: %s\n", zErrMsg, sql_dir);
	  sqlite3_free(zErrMsg);
	}

	/* Find and remove any dead file links */
	ret = sqlite3_exec(db, sql_files, cb_orphans, NULL, &zErrMsg);
	if ( ret != SQLITE_OK )
	{
	  DPRINTF(E_MAXDEBUG, L_SCANNER, "SQL error: %s\nBAD SQL: %s\n", zErrMsg, sql_files);
	  sqlite3_free(zErrMsg);
	}

	/* Rescan media_paths for new and/or modified files */
	for( media_path = media_dirs; media_path != NULL; media_path = media_path->next )
	{
		strncpyt(path, media_path->path, sizeof(path));
	  strncpyt(buf, media_path->path, sizeof(buf));
	  esc_name = escape_tag(basename(buf), 1);
	  rescan_insert_directory (esc_name, path);
	  free(esc_name);
	}
	DPRINTF(E_WARN, L_SCANNER, "Rescan completed\n");
}
