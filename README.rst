=================
NGINX Live Module
=================


.. contents::


Features
========

- multi-worker HTTP publish/subscribe streaming
- receives arbitrary live data POST'ed over HTTP
- passes it to all HTTP GET clients waiting on the same key in realtime
- may be used for MPEG-TS live streaming or HTTP push


Directives
==========


live_zone
---------

========== ====
*Syntax:*  ``live_zone zone=NAME:SIZE [persistent] [consistent] [flush]``
*Context:* http
========== ====

Creates a shared zone ``NAME`` with the ``SIZE`` for keeping the tree of
currently active streams.  This tree is used to check for duplicate
publishing and when trying to subscribe to a missing stream when not in
persistent mode.

The ``persistent`` parameter enables persistent mode.  In this mode a
subscriber is allowed to be connected to a stream not currently published
by any publisher.  By default, a new subscriber is only allowed to connect
to a currently active stream and is automatically disconnected when the
stream publisher disconnects.

The ``consistent`` parameter enables consistent mode.  In this mode it is not
allowed for a data stream fragment to be skipped if a subscriber is too slow.
Once a missing fragment is detected, subscriber is disconnected to avoid
data inconsistency.  By default, inconsistency is allowed, which is normal
for streaming MPEG-TS.  For stricter protocols this parameter should be
enabled to enforce consistency.

The ``flush`` parameter forces immediate flush of each data buffer sent
towards subscriber.  If not specified, output data is buffered as set by the
``postpone_output`` nginx directive.


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

This directive enables publish/subscribe streaming in the location.  The shared
zone ``NAME`` is used to account streams.  This shared zone should be created
by the ``live_zone`` directive.


live_methods
------------

========== ========
*Syntax:*  ``live_methods [GET] [POST]``
*Context:* location
========== ========

Specifies which methods are allowed in the location.  The method ``GET`` is
used to subscribe to a stream, while ``POST`` is used to publish a stream.


live_key
--------

========== ========
*Syntax:*  ``live_key KEY``
*Context:* location
*Default*  uri
========== ========

Specifies stream key.  By default, request URI is used as the key.


live_buffers
------------

========== ========
*Syntax:*  ``live_buffers NUMBER SIZE``
*Context:* location
*Default*  8 8192
========== ========


Example 1
=========

nginx.conf::

    events {}

    http {
        live_zone zone=foo:10m;

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


Example 2
=========

nginx.conf::

    events {}

    http {
        live_zone zone=foo:10m persistent flush;

        server {
            listen 8000;

            location ~ ^/pub/(?<name>[a-z0-9]*)$ {
                live foo;
                live_key $name;
                live_methods POST;
            }

            location ~ ^/sub/(?<name>[a-z0-9]*)$ {
                live foo;
                live_key $name;
                live_methods GET;
            }
        }
    }

subscribe::

    curl -N 127.0.0.1:8000/sub/foo

publish::

    curl -d 'message1' 127.0.0.1:8000/pub/foo
    curl -d 'message2' 127.0.0.1:8000/pub/foo
