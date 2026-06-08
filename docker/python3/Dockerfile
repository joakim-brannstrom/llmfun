FROM python:3.14-slim
RUN mkdir /workarea
WORKDIR /workarea

RUN apt-get update && apt-get install
RUN pip install --no-cache-dir scipy matplotlib pandas scikit-learn
