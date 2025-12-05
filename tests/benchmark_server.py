"""
Benchmark server using importable handlers for true sub-interpreter parallelism.
"""
from fasterapi.test_handlers import root, compute, get_user
from fasterapi.fastapi_compat import FastAPI
from fasterapi.http.server import Server
from fasterapi._fastapi_native import connect_route_registry_to_server

app = FastAPI()

# Register handlers from the importable module
app.get("/")(root)
app.get("/compute")(compute)
app.get("/user/{user_id}")(get_user)

if __name__ == "__main__":
    connect_route_registry_to_server()
    server = Server(port=9999, host="127.0.0.1")
    try:
        server.start()
    except KeyboardInterrupt:
        server.stop()
