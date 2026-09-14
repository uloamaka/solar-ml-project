import json

import pandas as pd
import paho.mqtt.client as mqtt

import db
from features import TZ

BROKER_HOST = 'localhost'
BROKER_PORT = 1883
TOPIC = 'solar/telemetry'


def on_message(client, userdata, msg):
    print(f"received on {msg.topic}: {msg.payload}")
    try:
        payload = json.loads(msg.payload.decode())
        power_w = float(payload['power_w'])
        timestamp = pd.Timestamp.now(tz=TZ).floor('h')
        db.insert_actual(timestamp, power_w)
        print(f"stored actual: {timestamp} -> {power_w} W")
    except Exception as e:
        print(f"failed to handle message: {e}")


def run():
    db.init_db()
    client = mqtt.Client()
    client.enable_logger()
    client.on_message = on_message
    client.connect(BROKER_HOST, BROKER_PORT)
    client.subscribe(TOPIC)
    client.loop_forever()


if __name__ == '__main__':
    run()