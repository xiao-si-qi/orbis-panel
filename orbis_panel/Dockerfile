# Uses the stock python image on purpose: this add-on is standard-library only,
# so there is no pip step and no dependency on PyPI at build time.
FROM python:3.12-alpine

COPY app /app
WORKDIR /app

EXPOSE 8099
CMD ["python3", "/app/main.py"]
