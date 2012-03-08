static struct mimeentry {
    const char     *name,
                   *type;
} mimetab[] = {
    {
    "html", "text/html; charset=UTF-8"}, {
    "htm", "text/html; charset=UTF-8"}, {
    "txt", "text/plain; charset=UTF-8"}, {
    "css", "text/css"}, {
    "ps", "application/postscript"}, {
    "pdf", "application/pdf"}, {
    "js", "application/javascript"}, {
    "gif", "image/gif"}, {
    "png", "image/png"}, {
    "jpeg", "image/jpeg"}, {
    "jpg", "image/jpeg"}, {
    "svg", "image/svg+xml"}, {
    "mpeg", "video/mpeg"}, {
    "mpg", "video/mpeg"}, {
    "avi", "video/x-msvideo"}, {
    "mov", "video/quicktime"}, {
    "qt", "video/quicktime"}, {
    "mp3", "audio/mpeg"}, {
    "ogg", "audio/ogg"}, {
    "wav", "audio/x-wav"}, {
    "epub", "application/epub+zip"}, {
    "dvi", "application/x-dvi"}, {
    "pac", "application/x-ns-proxy-autoconfig"}, {
    "sig", "application/pgp-signature"}, {
    "swf", "application/x-shockwave-flash"}, {
    "torrent", "application/x-bittorrent"}, {
    "tar", "application/x-tar"}, {
    "zip", "application/zip"}, {
    "dtd", "text/xml"}, {
    "xml", "text/xml"}, {
    "xbm", "image/x-xbitmap"}, {
    "xpm", "image/x-xpixmap"}, {
    "xwd", "image/x-xwindowdump"}, {
    "ico", "image/x-icon"}, {
    0, 0}};

static const char *default_mimetype = "application/octet-stream";

/*
 * Determine MIME type from file extension
 */
static const char *
getmimetype(char *url)
{
    char *ext = strrchr(url, '.');


    if (ext) {
        int             i;

        ext++;
        for (i = 0; mimetab[i].name; ++i) {
            if (!strcmp(mimetab[i].name, ext)) {
                return mimetab[i].type;
            }
        }
    }
    return default_mimetype;
}

