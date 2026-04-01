FROM redis:7-alpine

# Set time zone (IST) 
RUN apk add --no-cache tzdata
ENV TZ=Asia/Kolkata

# Copy custom configuration
COPY redis.conf /usr/local/etc/redis/redis.conf

# Start Redis with custom configuration
CMD ["redis-server", "/usr/local/etc/redis/redis.conf"]
