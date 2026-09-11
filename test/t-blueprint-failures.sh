#! /bin/sh
# Independent driver processes partition one feature-local countdown.
exec "${PYTHON:-python3}" ./inc/blueprint_runner.py pipeline
