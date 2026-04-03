FROM postgres:15-alpine

# Set timezone to IST
ENV TZ=Asia/Kolkata

# Add initialization scripts
# Note: These only run if the /var/lib/postgresql/data volume is empty
COPY init.sql /docker-entrypoint-initdb.d/

EXPOSE 5432
