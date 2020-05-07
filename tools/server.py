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

@app.route('/')
def root():
    return app.send_static_file('wifi.html')

@app.route("/ap.json", methods=["GET"])
def ap_json():
    with open(base_path+'\\tools\\ap.json') as json_file:
	    data = json.load(json_file)
    return jsonify(data)

@app.route("/status.json", methods=["GET"])
def status_json():
    with open(base_path+'\\tools\\status.json') as json_file:
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
		
@app.route("/event")
def stream():
    def eventStream():
        while True:
            yield "data: thisddddddddddd ddddddddddddd cccccccccccc bbbbbbbbb  aaaaaaa gggggggggg ddddddddd eeeeeeeeeee wwwwwwwww\n\n"
            sleep(1)
    
    return Response(eventStream(), mimetype="text/event-stream")

app.run(host='0.0.0.0', debug=True)
