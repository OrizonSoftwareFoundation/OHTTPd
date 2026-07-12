OHTTPd — Example static site
=============================

This directory is the default document root for OHTTPd.

Place any files or subdirectories here and they will be served
at the corresponding URL paths.

  public/
    index.html     ->  /
    style.css      ->  /style.css
    script.js      ->  /script.js
    data.json      ->  /data.json
    readme.txt     ->  /readme.txt

Subdirectories are served as-is.  If a subdirectory contains an
index.html, visiting the directory path will show that file.
