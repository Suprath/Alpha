FROM postgres:15-alpine

# Set time zone (IST) 
RUN apk add --no-cache tzdata
ENV TZ=Asia/Kolkata

# You can add custom configuration or init scripts here in the future
# e.g., COPY postgresql.conf /etc/postgresql/postgresql.conf
# e.g., COPY init.sql /docker-entrypoint-initdb.d/
