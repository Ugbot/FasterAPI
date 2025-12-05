# Cython declarations for C++ FastAPI components
# This file declares the C++ types and functions

from libcpp.string cimport string
from libcpp.vector cimport vector
from libcpp.unordered_map cimport unordered_map
from libcpp cimport bool as bool_t
from libcpp.memory cimport shared_ptr
from libcpp.utility cimport move

cdef extern from "Python.h":
    ctypedef struct PyObject

cdef extern from "http/schema_validator.h" namespace "fasterapi::http":
    cdef enum SchemaType:
        STRING
        INTEGER
        FLOAT
        BOOLEAN
        ARRAY
        OBJECT
        NULL_TYPE
        ANY

    # Wrapper functions for scoped enum conversion
    SchemaType schema_type_from_int(int value) except +
    int schema_type_to_int(SchemaType type) except +

    cdef cppclass Field:
        string name
        SchemaType type
        bool_t required

    cdef cppclass Schema:
        Schema(string name) except +
        void add_field(string name, SchemaType type, bool_t required) except +
        string get_name() const

    cdef cppclass SchemaRegistry:
        @staticmethod
        SchemaRegistry& instance()
        void register_schema(const string& name, shared_ptr[Schema] schema) except +
        shared_ptr[Schema] get_schema(const string& name) const
        bool_t has_schema(const string& name) const

cdef extern from "http/parameter_extractor.h" namespace "fasterapi::http":
    cdef cppclass ParameterExtractor:
        @staticmethod
        vector[string] extract_path_params(const string& pattern) except +
        @staticmethod
        unordered_map[string, string] get_query_params(const string& url) except +
        @staticmethod
        string url_decode(const string& encoded) except +

cdef extern from "http/route_metadata.h" namespace "fasterapi::http":
    cdef enum ParameterLocation:
        PATH
        QUERY
        BODY
        HEADER
        COOKIE

    # Wrapper functions for scoped enum conversion
    ParameterLocation param_location_from_int(int value) except +
    int param_location_to_int(ParameterLocation loc) except +

    cdef cppclass ParameterInfo:
        string name
        SchemaType type
        ParameterLocation location
        bool_t required
        string default_value
        string description

    cdef cppclass RouteMetadata:
        RouteMetadata(string method, string path) except +
        string method
        string path_pattern
        vector[ParameterInfo] parameters
        string request_body_schema
        string response_schema
        PyObject* handler
        string summary
        string description
        vector[string] tags

    cdef cppclass RouteRegistry:
        RouteRegistry() except +
        int register_route(RouteMetadata metadata) except +
        const vector[RouteMetadata]& get_all_routes() const

cdef extern from "http/openapi_generator.h" namespace "fasterapi::http":
    cdef cppclass OpenAPIGenerator:
        @staticmethod
        string generate(const vector[RouteMetadata]& routes,
                       const string& title,
                       const string& version,
                       const string& description) except +

cdef extern from "http/static_docs.h" namespace "fasterapi::http":
    cdef cppclass StaticDocs:
        @staticmethod
        string generate_swagger_ui_response(const string& openapi_url,
                                            const string& title) except +
        @staticmethod
        string generate_redoc_response(const string& openapi_url,
                                       const string& title) except +

cdef extern from "http/validation_error_formatter.h" namespace "fasterapi::http":
    cdef cppclass ValidationError:
        vector[string] loc
        string msg
        string type

    cdef cppclass ValidationResult:
        bool_t valid
        vector[ValidationError] errors

    cdef cppclass ValidationErrorFormatter:
        @staticmethod
        string format_as_json(const ValidationResult& result) except +
        @staticmethod
        string format_as_http_response(const ValidationResult& result) except +
