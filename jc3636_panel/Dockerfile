ARG BUILD_FROM
FROM $BUILD_FROM

RUN pip install --no-cache-dir flask requests

COPY app /app

EXPOSE 8099
CMD ["python3", "/app/main.py"]
