# Getting Started with FasterAPI

A complete tutorial for building your first high-performance web application with FasterAPI.

## What You'll Build

By the end of this guide, you'll have a complete REST API with:
- Fast HTTP server (30ns routing)
- PostgreSQL database integration
- Async/await support
- CRUD operations
- Transaction handling
- Error handling

Let's get started! 🚀

## Prerequisites

Before you begin, make sure you have:

- **Python 3.8+** installed
- **C++ compiler** (gcc, clang, or MSVC)
- **CMake 3.20+**
- **PostgreSQL** (optional, for database examples)

## Installation

### Step 1: Install FasterAPI

```bash
# Clone the repository
git clone https://github.com/bengamble/FasterAPI.git
cd FasterAPI

# Install with all features
pip install -e .[all]
```

This will:
1. Build the C++ core libraries
2. Compile Cython extensions
3. Install the Python package

It may take a few minutes the first time. Grab a coffee! ☕

### Step 2: Verify Installation

```bash
python -c "from fasterapi import App; print('✅ FasterAPI is ready!')"
```

If you see the success message, you're ready to go!

## Your First API

### Hello World

Create a file `main.py`:

```python
from fasterapi import App

# Create the application
app = App(port=8000)

# Define a route
@app.get("/")
def hello(req, res):
    return {"message": "Hello, FasterAPI!"}

# Run the server
if __name__ == "__main__":
    app.run()
```

Run it:

```bash
python main.py
```

Open your browser to `http://localhost:8000` and you should see:

```json
{"message": "Hello, FasterAPI!"}
```

**Congratulations!** You just built your first FasterAPI application! 🎉

### Understanding the Code

Let's break down what just happened:

```python
app = App(port=8000)
```
This creates a FasterAPI application listening on port 8000. Behind the scenes, FasterAPI starts a C++ HTTP server with kqueue (macOS) or epoll (Linux) for maximum performance.

```python
@app.get("/")
```
This decorator registers a route handler for GET requests to the root path (`/`).

```python
def hello(req, res):
```
Every FasterAPI route handler receives two arguments:
- `req`: The incoming Request object
- `res`: The outgoing Response object

```python
return {"message": "Hello, FasterAPI!"}
```
Return a dict and FasterAPI automatically converts it to JSON.

## Request Handling

### Path Parameters

Add this route to `main.py`:

```python
@app.get("/users/{user_id}")
def get_user(req, res):
    user_id = req.path_params["user_id"]
    return {
        "user_id": user_id,
        "name": f"User {user_id}"
    }
```

Try it:
```bash
curl http://localhost:8000/users/123
# {"user_id": "123", "name": "User 123"}
```

**Note:** Path parameters are strings by default. Convert them as needed:

```python
@app.get("/users/{user_id}")
def get_user(req, res):
    user_id = int(req.path_params["user_id"])  # Convert to int
    return {"user_id": user_id}
```

### Query Parameters

```python
@app.get("/search")
def search(req, res):
    query = req.query_params.get("q", "")
    limit = int(req.query_params.get("limit", 10))
    
    return {
        "query": query,
        "limit": limit,
        "results": []  # Your search logic here
    }
```

Try it:
```bash
curl "http://localhost:8000/search?q=python&limit=5"
# {"query": "python", "limit": 5, "results": []}
```

### Request Body (POST)

```python
from pydantic import BaseModel

class CreateUserRequest(BaseModel):
    name: str
    email: str
    age: int | None = None

@app.post("/users")
def create_user(req, res):
    # Parse JSON body
    data = req.json()
    
    # Validate with Pydantic
    user = CreateUserRequest(**data)
    
    # Your logic here (e.g., save to database)
    user_id = 123  # Simulated
    
    return {
        "id": user_id,
        "name": user.name,
        "email": user.email,
        "age": user.age
    }
```

Try it:
```bash
curl -X POST http://localhost:8000/users \
  -H "Content-Type: application/json" \
  -d '{"name": "Alice", "email": "alice@example.com", "age": 25}'
```

### Headers

```python
@app.get("/whoami")
def whoami(req, res):
    user_agent = req.headers.get("user-agent", "Unknown")
    auth = req.headers.get("authorization", "None")
    
    return {
        "user_agent": user_agent,
        "auth": auth
    }
```

### Custom Responses

```python
@app.get("/health")
def health(req, res):
    # JSON response (automatic)
    return {"status": "ok"}

@app.get("/text")
def text_response(req, res):
    # Text response
    res.set_header("Content-Type", "text/plain")
    return "Hello, plain text!"

@app.get("/html")
def html_response(req, res):
    # HTML response
    res.set_header("Content-Type", "text/html")
    return "<h1>Hello, HTML!</h1>"

@app.get("/custom-status")
def custom_status(req, res):
    res.status(201)
    return {"message": "Created"}
```

## Error Handling

### Simple Error Responses

```python
@app.get("/users/{user_id}")
def get_user(req, res):
    user_id = int(req.path_params["user_id"])
    
    # Simulate user not found
    if user_id > 1000:
        res.status(404)
        return {"error": "User not found"}
    
    return {"id": user_id, "name": f"User {user_id}"}
```

### Try/Except Error Handling

```python
@app.post("/users")
def create_user(req, res):
    try:
        data = req.json()
        user = CreateUserRequest(**data)
        
        # Your logic here
        return {"id": 123, "name": user.name}
        
    except ValueError as e:
        res.status(400)
        return {"error": f"Invalid data: {str(e)}"}
    except Exception as e:
        res.status(500)
        return {"error": "Internal server error"}
```

## Async/Await

FasterAPI supports async route handlers:

```python
import asyncio

@app.get("/slow")
async def slow_endpoint(req, res):
    # Simulate slow I/O operation
    await asyncio.sleep(1)
    return {"message": "This took 1 second"}

@app.get("/parallel")
async def parallel_endpoint(req, res):
    # Run multiple operations in parallel
    result1, result2 = await asyncio.gather(
        fetch_data_from_api_1(),
        fetch_data_from_api_2()
    )
    
    return {
        "api1": result1,
        "api2": result2
    }

async def fetch_data_from_api_1():
    await asyncio.sleep(0.1)
    return {"data": "from API 1"}

async def fetch_data_from_api_2():
    await asyncio.sleep(0.1)
    return {"data": "from API 2"}
```

## PostgreSQL Integration

This is where FasterAPI really shines! The C++ PostgreSQL driver is **4-10x faster** than pure Python drivers.

### Setup

First, install PostgreSQL:

```bash
# macOS
brew install postgresql
brew services start postgresql

# Ubuntu
sudo apt-get install postgresql
sudo service postgresql start
```

Create a test database:

```bash
createdb testdb

# Create a test table
psql testdb << EOF
CREATE TABLE users (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    email VARCHAR(100) UNIQUE NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

INSERT INTO users (name, email) VALUES 
    ('Alice', 'alice@example.com'),
    ('Bob', 'bob@example.com'),
    ('Charlie', 'charlie@example.com');
EOF
```

### Connecting to PostgreSQL

Update your `main.py`:

```python
from fasterapi import App, Depends
from fasterapi.pg import PgPool
from fasterapi.pg.compat import get_pg_factory
from pydantic import BaseModel

app = App(port=8000)

# Initialize the connection pool (do this once)
pool = PgPool(
    "postgres://localhost/testdb",
    min_size=2,
    max_size=20
)

# Create a dependency factory
get_pg = get_pg_factory(pool)

# Define models
class User(BaseModel):
    id: int | None = None
    name: str
    email: str

# Routes
@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    # Execute query
    result = pg.exec("SELECT * FROM users ORDER BY id")
    
    # Get all rows as dicts
    users = result.all()
    
    return {"users": users}

@app.get("/users/{user_id}")
def get_user(req, res, pg=Depends(get_pg)):
    user_id = int(req.path_params["user_id"])
    
    # Parameterized query (safe from SQL injection)
    result = pg.exec(
        "SELECT * FROM users WHERE id = $1",
        user_id
    )
    
    # Get one row or None
    user = result.one_or_none()
    
    if user is None:
        res.status(404)
        return {"error": "User not found"}
    
    return {"user": user}

@app.post("/users")
def create_user(req, res, pg=Depends(get_pg)):
    data = req.json()
    user = User(**data)
    
    # Insert and return the new ID
    user_id = pg.exec(
        "INSERT INTO users (name, email) VALUES ($1, $2) RETURNING id",
        user.name,
        user.email
    ).scalar()  # Get single value
    
    return {
        "id": user_id,
        "name": user.name,
        "email": user.email
    }

@app.put("/users/{user_id}")
def update_user(req, res, pg=Depends(get_pg)):
    user_id = int(req.path_params["user_id"])
    data = req.json()
    
    # Update
    pg.exec(
        "UPDATE users SET name = $1, email = $2 WHERE id = $3",
        data.get("name"),
        data.get("email"),
        user_id
    )
    
    return {"message": "User updated"}

@app.delete("/users/{user_id}")
def delete_user(req, res, pg=Depends(get_pg)):
    user_id = int(req.path_params["user_id"])
    
    pg.exec("DELETE FROM users WHERE id = $1", user_id)
    
    return {"message": "User deleted"}

# Lifecycle events
@app.on_event("startup")
def startup():
    print("🚀 Server starting")
    print(f"📊 PostgreSQL pool: {pool.min_size}-{pool.max_size} connections")

@app.on_event("shutdown")
def shutdown():
    print("🛑 Server shutting down")
    pool.close()

if __name__ == "__main__":
    app.run()
```

### Test Your API

```bash
# List all users
curl http://localhost:8000/users

# Get a specific user
curl http://localhost:8000/users/1

# Create a user
curl -X POST http://localhost:8000/users \
  -H "Content-Type: application/json" \
  -d '{"name": "Dave", "email": "dave@example.com"}'

# Update a user
curl -X PUT http://localhost:8000/users/1 \
  -H "Content-Type: application/json" \
  -d '{"name": "Alice Smith", "email": "alice.smith@example.com"}'

# Delete a user
curl -X DELETE http://localhost:8000/users/1
```

### Transactions

For operations that need to be atomic:

```python
from fasterapi.pg import TxIsolation

@app.post("/transfer")
def transfer_money(req, res, pg=Depends(get_pg)):
    data = req.json()
    from_id = data["from_id"]
    to_id = data["to_id"]
    amount = data["amount"]
    
    try:
        # Start a transaction
        with pg.tx(isolation=TxIsolation.serializable, retries=3) as tx:
            # Check balance
            balance = tx.exec(
                "SELECT balance FROM accounts WHERE id = $1 FOR UPDATE",
                from_id
            ).scalar()
            
            if balance < amount:
                raise ValueError("Insufficient funds")
            
            # Deduct from sender
            tx.exec(
                "UPDATE accounts SET balance = balance - $1 WHERE id = $2",
                amount, from_id
            )
            
            # Add to recipient
            tx.exec(
                "UPDATE accounts SET balance = balance + $1 WHERE id = $2",
                amount, to_id
            )
            
            # Transaction automatically commits here
        
        return {"message": "Transfer successful"}
        
    except ValueError as e:
        res.status(400)
        return {"error": str(e)}
    except Exception as e:
        res.status(500)
        return {"error": "Transaction failed"}
```

**Features:**
- Automatic commit/rollback
- Configurable isolation levels
- Automatic retry on serialization failure
- `FOR UPDATE` locking support

### Bulk Operations (COPY)

For inserting large amounts of data:

```python
@app.post("/users/bulk")
def bulk_import(req, res, pg=Depends(get_pg)):
    users = req.json()["users"]
    
    # Use COPY for fast bulk insert
    with pg.copy_in("COPY users(name, email) FROM stdin CSV") as pipe:
        for user in users:
            line = f"{user['name']},{user['email']}\n"
            pipe.write(line.encode())
    
    return {"imported": len(users)}
```

Test it:
```bash
curl -X POST http://localhost:8000/users/bulk \
  -H "Content-Type: application/json" \
  -d '{"users": [
    {"name": "User1", "email": "user1@example.com"},
    {"name": "User2", "email": "user2@example.com"},
    {"name": "User3", "email": "user3@example.com"}
  ]}'
```

**COPY is 10-100x faster than individual INSERTs for bulk data!**

## Dependency Injection

Dependency injection helps you share resources (like database connections) across routes:

```python
from fasterapi import Depends

# Define a dependency
def get_current_user(req):
    """Extract and validate user from Authorization header"""
    token = req.headers.get("authorization", "").replace("Bearer ", "")
    
    if not token:
        raise ValueError("No authorization token")
    
    # In a real app, verify the token here
    # For now, just extract user_id from token
    user_id = int(token)
    
    return {"id": user_id, "name": f"User {user_id}"}

# Use the dependency
@app.get("/me")
def get_me(req, res, user=Depends(get_current_user)):
    return {"user": user}

@app.get("/my-posts")
def get_my_posts(req, res, user=Depends(get_current_user), pg=Depends(get_pg)):
    result = pg.exec(
        "SELECT * FROM posts WHERE user_id = $1",
        user["id"]
    )
    return {"posts": result.all()}
```

Test it:
```bash
# No token - should fail
curl http://localhost:8000/me

# With token
curl http://localhost:8000/me -H "Authorization: Bearer 123"
```

## Middleware

Middleware lets you process requests before they reach your handlers:

```python
import time

@app.add_middleware
def timing_middleware(req, res):
    """Add request timing"""
    start = time.time()
    
    # Note: Current middleware is simple
    # Full request/response wrapping coming in v0.3
    
    print(f"Request: {req.method} {req.path}")
    duration = time.time() - start
    print(f"Duration: {duration:.3f}s")

@app.add_middleware
def auth_middleware(req, res):
    """Simple authentication"""
    # Skip auth for public routes
    if req.path in ["/", "/health"]:
        return
    
    token = req.headers.get("authorization")
    if not token:
        res.status(401)
        return {"error": "Unauthorized"}
```

## Application Structure

For larger projects, organize your code:

```
my_project/
├── main.py              # Application entry point
├── models.py            # Pydantic models
├── database.py          # Database setup
├── routers/
│   ├── __init__.py
│   ├── users.py         # User routes
│   └── posts.py         # Post routes
├── dependencies.py      # Shared dependencies
└── config.py            # Configuration
```

**database.py:**
```python
from fasterapi.pg import PgPool
from fasterapi.pg.compat import get_pg_factory

pool = PgPool(
    "postgres://localhost/mydb",
    min_size=2,
    max_size=20
)

get_pg = get_pg_factory(pool)
```

**routers/users.py:**
```python
from fasterapi import Depends
from database import get_pg

def register_routes(app):
    @app.get("/users")
    def list_users(req, res, pg=Depends(get_pg)):
        result = pg.exec("SELECT * FROM users")
        return {"users": result.all()}
    
    @app.get("/users/{user_id}")
    def get_user(req, res, pg=Depends(get_pg)):
        user_id = int(req.path_params["user_id"])
        result = pg.exec("SELECT * FROM users WHERE id = $1", user_id)
        user = result.one_or_none()
        
        if not user:
            res.status(404)
            return {"error": "User not found"}
        
        return {"user": user}
```

**main.py:**
```python
from fasterapi import App
from database import pool
from routers import users, posts

app = App(port=8000)

# Register routes
users.register_routes(app)
posts.register_routes(app)

@app.on_event("startup")
def startup():
    print("🚀 Server starting")

@app.on_event("shutdown")
def shutdown():
    pool.close()

if __name__ == "__main__":
    app.run()
```

## Next Steps

Congratulations! You now know the basics of FasterAPI. Here's what to explore next:

### Learn More
- **[Migration from FastAPI](MIGRATION_FROM_FASTAPI.md)** - If you're coming from FastAPI
- **[Performance Guide](performance.md)** - Optimization tips
- **[Architecture](architecture.md)** - How FasterAPI works internally

### Explore Features
- **[MCP Protocol](mcp/README.md)** - Model Context Protocol for AI tools
- **[WebRTC](webrtc.md)** - Real-time communication (experimental)
- **[HTTP/2](http2.md)** - HTTP/2 server push (coming soon)

### Examples
Check out the [examples/](../examples/) directory:
- `basic_app.py` - Simple REST API
- `complete_demo.py` - Full-featured application
- `async_http_demo.py` - Async/await patterns
- `production_app.py` - Production-ready setup

### Get Help
- **GitHub Issues**: https://github.com/bengamble/FasterAPI/issues
- **Discussions**: https://github.com/bengamble/FasterAPI/discussions
- **Examples**: Check the examples/ directory

## Tips & Best Practices

### 1. Always Use Parameterized Queries

❌ **Bad (SQL injection risk):**
```python
pg.exec(f"SELECT * FROM users WHERE email = '{email}'")
```

✅ **Good:**
```python
pg.exec("SELECT * FROM users WHERE email = $1", email)
```

### 2. Initialize Pool Once

❌ **Bad:**
```python
@app.get("/users")
def list_users(req, res):
    pool = PgPool("postgres://localhost/mydb")  # New pool every request!
    pg = pool.get()
    return pg.exec("SELECT * FROM users").all()
```

✅ **Good:**
```python
# At module level
pool = PgPool("postgres://localhost/mydb", min_size=2, max_size=20)
get_pg = get_pg_factory(pool)

@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    return pg.exec("SELECT * FROM users").all()
```

### 3. Type Convert Path Parameters

❌ **Bad:**
```python
@app.get("/users/{user_id}")
def get_user(req, res):
    user_id = req.path_params["user_id"]  # This is a string!
    # Will fail if you expect an int
```

✅ **Good:**
```python
@app.get("/users/{user_id}")
def get_user(req, res):
    user_id = int(req.path_params["user_id"])
```

### 4. Use Transactions for Multiple Writes

❌ **Bad:**
```python
pg.exec("UPDATE accounts SET balance = balance - 100 WHERE id = 1")
pg.exec("UPDATE accounts SET balance = balance + 100 WHERE id = 2")
# If second query fails, first already committed!
```

✅ **Good:**
```python
with pg.tx() as tx:
    tx.exec("UPDATE accounts SET balance = balance - 100 WHERE id = 1")
    tx.exec("UPDATE accounts SET balance = balance + 100 WHERE id = 2")
    # Atomic: both succeed or both fail
```

### 5. Close Pool on Shutdown

✅ **Always:**
```python
@app.on_event("shutdown")
def shutdown():
    pool.close()
```

## Troubleshooting

### "Connection refused" error

Make sure PostgreSQL is running:
```bash
# macOS
brew services start postgresql

# Ubuntu
sudo service postgresql start
```

### "relation does not exist" error

Create your tables:
```bash
psql mydb < schema.sql
```

### Import errors

Make sure FasterAPI is installed:
```bash
pip install -e .[all]
```

### Slow builds

The first build takes time. Subsequent builds are incremental and much faster.

## Conclusion

You've learned:
- ✅ How to create a FasterAPI application
- ✅ Request handling (path, query, body)
- ✅ PostgreSQL integration
- ✅ Async/await
- ✅ Dependency injection
- ✅ Transactions and bulk operations
- ✅ Error handling
- ✅ Project structure

You're now ready to build high-performance web applications with FasterAPI! 🚀

**Happy coding!**

