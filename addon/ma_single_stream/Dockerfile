# An explicit base image: recent Supervisors no longer pass BUILD_FROM to a
# local add-on without a build.yaml. The script only needs the standard library.
FROM python:3.13-alpine
COPY single_stream.py /single_stream.py
CMD ["python3", "-u", "/single_stream.py"]
