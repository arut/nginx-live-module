=================
NGINX Live Module
=================


.. contents::


Features
========

- receives arbitrary live data POST'ed over HTTP
- passes it to all HTTP GET clients waiting on the same key in realtime
- works with multiple workers
- may operate in consistent or inconsistent mode
- may be used for MPEG-TS live streaming or any type of HTTP push


Directives
==========


live_zone
---------

========== ====
*Syntax:*  ``live_zone zone=NAME:SIZE [persistent] [consistent] [flush]``
*Context:* http
========== ====

live_buffer_size
----------------

========== ====
*Syntax:*  ``live_buffer_size SIZE``
*Context:* http
========== ====

live
----

========== ========
*Syntax:*  ``live NAME``
*Context:* location
========== ========

live_methods
------------

========== ========
*Syntax:*  ``live_methods [GET] [POST]``
*Context:* location
========== ========

live_key
--------

========== ========
*Syntax:*  ``live_key KEY``
*Context:* location
*Default*  uri
========== ========

live_buffers
------------

========== ========
*Syntax:*  ``live_buffers NUMBER SIZE``
*Context:* location
*Default*  8 8192
========== ========


Example
=======

nginx.conf::

    http {
        live_zone zone=foo:10m persistent flush;

        server {
            listen 8000;

            location / {
                live foo;
                live_methods GET POST;

                # enable endless request body
                client_max_body_size 0;
            }
        }
    }

publish MPEG-TS::

    ffmpeg -re -i ~/Movies/sintel.mp4 -c copy -f mpegts http://127.0.0.1:8000/sintel

play MPEG-TS::

    ffplay http://127.0.0.1:8000/sintel
