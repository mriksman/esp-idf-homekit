#!/usr/bin/env python

from flask import Flask, request, jsonify, Response
from time import sleep

import os
import os.path
import logging
import json

base_path = os.path.join(os.path.dirname(os.path.realpath(__file__)), '..')
app = Flask(
    __name__,
    static_folder=os.path.join(base_path, 'web')
)

counter = 0
update = "Initial"

@app.route('/')
def root():
    return app.send_static_file('wifi.html')

@app.route("/ap.json", methods=["GET"])
def ap_json():
    with open(base_path+'\\tools\\ap.json') as json_file:
	    data = json.load(json_file)
    return jsonify(data)

@app.route("/getlights.json", methods=["GET"])
def getlights_json():
    with open(base_path+'\\tools\\getlights.json') as json_file:
	    data = json.load(json_file)
    return jsonify(data)

@app.route("/connect.json", methods=["POST"])
def connect_json():
    # Validate the request body contains JSON
    if request.is_json:
        # Parse the JSON into a Python dictionary
        req = request.get_json()
        # Print the dictionary
        print(req)
        # Return a string along with an HTTP status code
        return "JSON received!", 200
    else:
        # The request body wasn't JSON so return a 400 HTTP status code
        return "Request was not JSON", 400

@app.route("/restart.json", methods=["POST"])
def restart_json():
    if request.is_json:
        req = request.get_json()
        if 'update' in req and req['update'] == True:
            global update
            update = "Connected.."
        print(req)
        return "JSON received!", 200
    else:
        return "Request was not JSON", 400

@app.route("/event")
def stream():
    def eventStream():
        with app.app_context():		
            global counter
            global update
            while True:
                counter += 1
                if counter == 100:
                    counter=0
                yield "data: this is a    really long     debug string that will be longer than the width of the log display\n\n"
                with open(base_path+'\\tools\\status.json') as json_file:
	                data = json.loads(json_file.read())	
                yield "event: status\ndata:" + json.dumps(data) + "\n\n"
                yield "event: firmware\ndata:{\"version\":\"abcde-3443\"}\n\n"
                yield "event: update\ndata:{\"progress\":\"" + str(counter) + "\", \"status\":\"" + update + "\"}\n\n"
                sleep(5)
    
    return Response(eventStream(), mimetype="text/event-stream")

# /send URI would be on the 'boot' partition to accept 'main' update file
@app.route("/send", methods=["POST"])
def send_update():
    global update
    update = "Downloading..."
    print(request.content_length)
    with open(base_path+'\\tools\\test.bin', "wb") as fp:
        fp.write(request.data)
    return "File downloaded", 200

app.run(host='0.0.0.0', debug=True)
