FROM questdb/questdb:latest

# Set time zone (IST) 
USER root
RUN apt-get update && apt-get install -y tzdata && rm -rf /var/lib/apt/lists/*
ENV TZ=Asia/Kolkata
USER questdb

# Custom configuration can be added here in the future
# e.g., COPY server.conf /var/lib/questdb/conf/server.conf
