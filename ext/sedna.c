/*
 * Copyright 2008 Voormedia B.V.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ========================================================================
 *
 * Ruby extension library providing a client API to the Sedna native XML
 * database management system, based on the official Sedna C driver.
 *
 * This file contains the Ruby C-extension.
 *
 * ========================================================================
 */

#include <string.h>
#include "ruby.h"
#include "libsedna.h"

// Size of the query result read buffer.
#define RESULT_BUF_LEN 8192

// Size of the load_document buffer.
#define LOAD_BUF_LEN 8192

// Use this macro to fool RDoc and hide some aliases.
#define rb_define_undocumented_alias(kl, new, old) rb_define_alias(kl, new, old)

// Macro for verification of return values of the Sedna API + error handling.
#define VERIFY_RES(expected, real, conn) if(real != expected) sedna_err(conn, real)

// Macro for setting autocommit values on connection conn.
#define SEDNA_AUTOCOMMIT_ENABLE(conn) sedna_autocommit(conn, SEDNA_AUTOCOMMIT_ON)
#define SEDNA_AUTOCOMMIT_DISABLE(conn) sedna_autocommit(conn, SEDNA_AUTOCOMMIT_OFF)
#define SWITCH_SEDNA_AUTOCOMMIT(conn, val) if(val) SEDNA_AUTOCOMMIT_ENABLE(conn); else SEDNA_AUTOCOMMIT_DISABLE(conn)

// Define the protocol version number as string.
#define PROTO_STRINGIFY(s) #s
#define PROTO_TO_STRING(s) PROTO_STRINGIFY(s)
#define PROTOCOL_VERSION PROTO_TO_STRING(SE_CURRENT_SOCKET_PROTOCOL_VERSION_MAJOR) "." PROTO_TO_STRING(SE_CURRENT_SOCKET_PROTOCOL_VERSION_MINOR)

// Default connection arguments.
#define DEFAULT_HOST "localhost"
#define DEFAULT_DB "test"
#define DEFAULT_USER "SYSTEM"
#define DEFAULT_PW "MANAGER"

// Instance variable names.
#define IV_HOST "@host"
#define IV_DB "@database"
#define IV_USER "@username"
#define IV_PW "@password"
#define IV_AUTOCOMMIT "@autocommit"
#define IV_MUTEX "@mutex"

// Define a shorthand for the common SednaConnection structure.
typedef struct SednaConnection SC;

#ifdef HAVE_RB_THREAD_BLOCKING_REGION
	#define NON_BLOCKING
	#define SEDNA_BLOCKING Qfalse

	struct SednaQuery {
		void *conn;
		void *query;
	};
	typedef struct SednaQuery SQ;
#else
	#define SEDNA_BLOCKING Qtrue
#endif

// Ruby classes.
static VALUE cSedna;
//static VALUE cSednaSet; // Stick to Array for result sets.
static VALUE cSednaException;
static VALUE cSednaAuthError;
static VALUE cSednaConnError;
static VALUE cSednaTrnError;


// Common functions =======================================================

// Test the last error message for conn, and raise an exception if there is one.
// The type of the exception is based on the result of the function that was
// called that generated the error and should be passed as res.
static void sedna_err(SC *conn, int res)
{
	VALUE exception;
	const char *msg;
	char *err, *details, *p;

	switch(res) {
		case SEDNA_AUTHENTICATION_FAILED:
			exception = cSednaAuthError; break;
		case SEDNA_OPEN_SESSION_FAILED:
		case SEDNA_CLOSE_SESSION_FAILED:
			exception = cSednaConnError; break;
		case SEDNA_BEGIN_TRANSACTION_FAILED:
		case SEDNA_ROLLBACK_TRANSACTION_FAILED:
		case SEDNA_COMMIT_TRANSACTION_FAILED:
			exception = cSednaTrnError; break;
		case SEDNA_ERROR:
		default:
			exception = cSednaException;
	}

	msg = SEgetLastErrorMsg(conn);
	err = strstr(msg, "\n");
	details = strstr(err, "\nDetails: ");

	if(err != NULL) {
		err++;
		if((p = strstr(err, "\n")) != NULL) strncpy(p, "\0", 1);
	} else {
		err = "Unknown error.";
	}

	if(details != NULL) {
		details += 10;
		while((p = strstr(details, "\n")) != NULL) strncpy(p, " ", 1);
		rb_raise(exception, "%s (%s)", err, details);
	} else {
		rb_raise(exception, "%s", err);
	}
}

// Retrieve the SednaConnection struct from the Ruby Sedna object obj.
static SC* sedna_struct(VALUE obj)
{
	SC *conn;
	Data_Get_Struct(obj, SC, conn);
	return conn;
}

// Close the Sedna connection and free memory of the SednaConnection struct.
// Called at GC.
static void sedna_free(SC *conn)
{
	if(SEconnectionStatus(conn) != SEDNA_CONNECTION_CLOSED) SEclose(conn);
	free(conn);
}

// Mark any references to other objects for Ruby GC (if any).
static void sedna_mark(SC *conn)
{ /* Unused. */ }

// Connect to the server.
static void sedna_connect(SC *conn, char *host, char *db, char *user, char *pw)
{
	int res = SEconnect(conn, host, db, user, pw);
	if(res != SEDNA_SESSION_OPEN) {
		// We have to set the connection status to closed explicitly here,
		// because the GC routine sedna_free() will test for this status, but
		// the socket is already closed by SEconnect(). If we do not change the
		// status, sedna_free() will attempt to close the connection again by
		// calling SEclose(), which will definitely lead to unpredictable
		// results.
		conn->isConnectionOk = SEDNA_CONNECTION_CLOSED;
		sedna_err(conn, res);
	}
}

// Close the connection to the server.
static void sedna_close(SC *conn)
{
	int res;
	if(SEconnectionStatus(conn) != SEDNA_CONNECTION_CLOSED) {
		res = SEclose(conn);
		VERIFY_RES(SEDNA_SESSION_CLOSED, res, conn);
	}
}

#ifdef NON_BLOCKING
static int sedna_blocking_execute(SQ *q)
{
	return SEexecute(q->conn, q->query);
}

static int sedna_execute(SQ *q)
{
	return rb_thread_blocking_region((void*)sedna_blocking_execute, q, RUBY_UBF_IO, NULL);
}
#endif

// Read one record completely and return it as a Ruby String object.
static VALUE sedna_read(SC *conn, int strip_n)
{
	int bytes_read = 0;
	char buffer[RESULT_BUF_LEN];
	VALUE str = rb_str_buf_new(0);
	OBJ_TAINT(str);

	do {
		bytes_read = SEgetData(conn, buffer, RESULT_BUF_LEN - 1);
		if(bytes_read == SEDNA_ERROR) {
			sedna_err(conn, SEDNA_ERROR);
		} else {
			if(bytes_read > 0) {
				if(strip_n) {
					// Strange bug adds newlines to beginning of every result
					// except the first. Strip them! This a known issue in the
					// network protocol and serialization mechanism.
					// See: http://sourceforge.net/mailarchive/forum.php?thread_name=3034886f0812030132v3bbd8e2erd86480d3dc640664%40mail.gmail.com&forum_name=sedna-discussion
					rb_str_buf_cat(str, buffer + 1, bytes_read - 1);
					// Do not strip newlines from subsequent buffer reads.
					strip_n = 0;
				} else {
					rb_str_buf_cat(str, buffer, bytes_read);
				}
			}
		}
	} while(bytes_read > 0);

	return str;
}

// Iterate over all records and add them to a Ruby Array.
static VALUE sedna_get_results(SC *conn)
{
	int res, strip_n = 0;
	// Can be replaced with: rb_funcall(cSednaSet, rb_intern("new"), 0, NULL);
	VALUE set = rb_ary_new();

	while((res = SEnext(conn)) != SEDNA_RESULT_END) {
		if(res == SEDNA_ERROR) sedna_err(conn, res);
		// Set strip_n to 1 for all results except the first. This will cause
		// sedna_read() an incorrect newline that is prepended to these results.
		rb_ary_push(set, sedna_read(conn, strip_n));
		if(!strip_n) strip_n = 1;
	};

	return set;
}

// Enable or disable autocommit.
static void sedna_autocommit(SC *conn, int value)
{
	int res = SEsetConnectionAttr(conn, SEDNA_ATTR_AUTOCOMMIT, (void *)&value, sizeof(int));
	VERIFY_RES(SEDNA_SET_ATTRIBUTE_SUCCEEDED, res, conn);
}

// Begin a transaction.
static void sedna_tr_begin(SC *conn)
{
	int res = SEbegin(conn);
	VERIFY_RES(SEDNA_BEGIN_TRANSACTION_SUCCEEDED, res, conn);
}

// Commit a transaction.
static void sedna_tr_commit(SC *conn)
{
	int res = SEcommit(conn);
	VERIFY_RES(SEDNA_COMMIT_TRANSACTION_SUCCEEDED, res, conn);
}

// Rollback a transaction.
static void sedna_tr_rollback(SC *conn)
{
	int res = SErollback(conn);
	VERIFY_RES(SEDNA_ROLLBACK_TRANSACTION_SUCCEEDED, res, conn);
}


// Functions available from Ruby ==========================================

// Alocates memory for a SednaConnection struct.
static VALUE cSedna_s_new(VALUE klass)
{
	SC *conn, init = SEDNA_CONNECTION_INITIALIZER;
	conn = ALLOC(SC);
	if(conn == NULL) rb_raise(rb_eNoMemError, "Could not allocate memory.");
	memcpy(conn, &init, sizeof(init));

	return Data_Wrap_Struct(klass, sedna_mark, sedna_free, conn);
}

/* :nodoc:
 *
 * Initialize a new instance of Sedna. Undocumented, because Sedna.connect should
 * be used instead.
 */
static VALUE cSedna_initialize(VALUE self, VALUE options)
{
	VALUE host_k, db_k, user_k, pw_k, host_v, db_v, user_v, pw_v;
	char *host, *db, *user, *pw;
	SC *conn = sedna_struct(self);

	// Ensure the argument is a Hash.
	Check_Type(options, T_HASH);
	
	// Store the symbols of the valid hash keys.
	host_k = ID2SYM(rb_intern("host"));
	db_k = ID2SYM(rb_intern("database"));
	user_k = ID2SYM(rb_intern("username"));
	pw_k = ID2SYM(rb_intern("password"));

	// Get the connection details or set them to the default values if not given.
	if(NIL_P(host_v = rb_hash_aref(options, host_k))) host = DEFAULT_HOST; else host = STR2CSTR(host_v);
	if(NIL_P(db_v = rb_hash_aref(options, db_k))) db = DEFAULT_DB; else db = STR2CSTR(db_v);
	if(NIL_P(user_v = rb_hash_aref(options, user_k))) user = DEFAULT_USER; else user = STR2CSTR(user_v);
	if(NIL_P(pw_v = rb_hash_aref(options, pw_k))) pw = DEFAULT_PW; else pw = STR2CSTR(pw_v);
	
	// Save all connection details to instance variables.
	rb_iv_set(self, IV_HOST, rb_str_new2(host));
	rb_iv_set(self, IV_DB, rb_str_new2(db));
	rb_iv_set(self, IV_USER, rb_str_new2(user));
	rb_iv_set(self, IV_PW, rb_str_new2(pw));

	// Connect to the database.
	sedna_connect(conn, host, db, user, pw);

	// Initialize @autocommit to true.
	rb_iv_set(self, IV_AUTOCOMMIT, Qtrue);

#ifdef NON_BLOCKING
	// Create a mutex if this build supports non-blocking queries.
	rb_iv_set(self, IV_MUTEX, rb_mutex_new());
#endif

	return self;
}

/*
 * call-seq:
 *   sedna.close -> nil
 *
 * Closes an open Sedna connection. If the connection is already closed when
 * this method is called, nothing happens. A Sedna::ConnectionError is raised
 * if the connection was open but could not be closed.
 */
static VALUE cSedna_close(VALUE self)
{
	SC *conn = sedna_struct(self);
	
	// Ensure the connection is closed.
	sedna_close(conn);
	
	// Always return nil if successful.
	return Qnil;
}

/*
 * call-seq:
 *   sedna.reset -> nil
 *
 * Closes an open Sedna connection and reconnects. If the connection is already
 * closed when this method is called, the connection is just reestablished. When
 * reconnecting, the same connection details are used that were given when initially
 * connecting with the connect method.
 *
 * If the connection could not be closed or reopened, a Sedna::ConnectionError is
 * raised. If the authentication fails when reconnecting, a
 * Sedna::AuthenticationError is raised.
 */
static VALUE cSedna_reset(VALUE self)
{
	char *host, *db, *user, *pw;
	SC *conn = sedna_struct(self);
	
	// First ensure the current connection is closed.
	sedna_close(conn);

	// Retrieve stored connection details.
	host = STR2CSTR(rb_iv_get(self, IV_HOST));
	db = STR2CSTR(rb_iv_get(self, IV_DB));
	user = STR2CSTR(rb_iv_get(self, IV_USER));
	pw = STR2CSTR(rb_iv_get(self, IV_PW));
	
	// Connect again.
	sedna_connect(conn, host, db, user, pw);

	// Always return nil if successful.
	return Qnil;
}

/*
 * call-seq:
 *   Sedna.connect(details) -> Sedna instance
 *   Sedna.connect(details) {|sedna| ... } -> nil
 *
 * Establishes a new connection to a \Sedna XML database. Accepts a hash that
 * describes which database to \connect to.
 *
 * If a block is given, the block is executed if a connection was successfully
 * established. The connection is closed at the end of the block. If called
 * without a block, a Sedna object that represents the connection is returned.
 * The connection should be closed by calling Sedna#close.
 *
 * If a connection cannot be initiated, a Sedna::ConnectionError is raised.
 * If the authentication fails, a Sedna::AuthenticationError is raised.
 *
 * ==== Valid connection details keys
 *
 * * <tt>:host</tt> - Host name or IP address to which to \connect to (defaults to +localhost+).
 * * <tt>:database</tt> - Name of the database to \connect to (defaults to +test+).
 * * <tt>:username</tt> - User name to authenticate with (defaults to +SYSTEM+).
 * * <tt>:password</tt> - Password to authenticate with (defaults to +MANAGER+).
 *
 * ==== Examples
 *
 * Call without a block and close the connection afterwards.
 *   sedna = Sedna.connect(:database => "my_db", :host => "my_host")
 *   # Query the database and close afterwards.
 *   sedna.close
 *
 * Call with a block. The connection is closed automatically.
 *   Sedna.connect(:database => "my_db", :host => "my_host") do |sedna|
 *     # Query the database.
 *     # The connection is closed automatically.
 *   end
 */
static VALUE cSedna_s_connect(VALUE klass, VALUE options)
{
	int status;
	
	// Create a new instance.
	VALUE obj = rb_funcall(klass, rb_intern("new"), 1, options);

	if(rb_block_given_p()) {
		// If a block is given, yield the instance, and make sure we always return...
		rb_protect(rb_yield, obj, &status);
		
		// ...to ensure that the connection is closed afterwards.
		cSedna_close(obj);
		
		// Re-raise any exception.
		if(status != 0) rb_jump_tag(status);

		// Always return nil if successful.
		return Qnil;
	} else {
		// If no block is given, simply return the instance.
		return obj;
	}
}

/*
 * call-seq:
 *   Sedna.version -> string
 *
 * Returns the current version of the Sedna client protocol.
 */
static VALUE cSedna_s_version(VALUE klass)
{
	return rb_str_new2(PROTOCOL_VERSION);
}

/*
 * call-seq:
 *   Sedna.blocking? -> true or false
 *
 * Returns +true+ if querying the database with Sedna#execute will block other
 * threads. Returns +false+ if multiple queries can be run in different threads
 * simultaneously. \Sedna will not block other threads when compiled against
 * Ruby 1.9.
 */
static VALUE cSedna_s_blocking(VALUE klass)
{
	return SEDNA_BLOCKING;
}

/*
 * call-seq:
 *   sedna.connected? -> true or false
 *
 * Returns +true+ if the connection is connected and functioning properly. Returns
 * +false+ if the connection has been closed.
 */
static VALUE cSedna_connected(VALUE self)
{
	int res;
	SC *conn = sedna_struct(self);
	
	// Return true if the connection status is OK. This only indicates that the
	// client still thinks it is connected.
	return (SEconnectionStatus(conn) == SEDNA_CONNECTION_OK) ? Qtrue : Qfalse;
}

/*
 * call-seq:
 *   sedna.execute(query) -> array or nil
 *   sedna.query(query) -> array or nil
 *
 * Executes the given +query+ against a \Sedna database. Returns an array if the
 * given query is a select query. The elements of the array are strings that
 * correspond to each result in the result set. If the query is an update query
 * or a (bulk) load query, +nil+ is returned. When attempting to \execute a
 * query on a closed connection, a Sedna::ConnectionError will be raised. A
 * Sedna::Exception is raised if the query fails or is invalid.
 *
 * This method does not block other threads in Ruby 1.9 -- database queries that
 * are run in different threads with different connections will run concurrently.
 * You can use Sedna.blocking? to verify if the extension supports non-blocking
 * behaviour. Database queries run from different threads, but on the same
 * connection will still block and be executed serially.
 *
 * ==== Examples
 *
 * Create a new document.
 *   sedna.execute "create document 'mydoc'"
 *     #=> nil
 * Update the newly created document with a root node.
 *   sedna.execute "update insert <message>Hello world!</message> into doc('mydoc')"
 *     #=> nil
 * Select a node in a document using XPath.
 *   sedna.execute "doc('mydoc')/message/text()"
 *     #=> ["Hello world!"]
 *
 * ==== Further reading
 *
 * For more information about \Sedna's database query syntax and support, see the
 * <i>Database language</i> section of the official documentation of the
 * \Sedna project at http://modis.ispras.ru/sedna/progguide/ProgGuidese2.html
 */
static VALUE cSedna_execute(VALUE self, VALUE query)
{
	int res;
	SC *conn = sedna_struct(self);

	// Verify that the connection is OK.
	if(SEconnectionStatus(conn) != SEDNA_CONNECTION_OK) rb_raise(cSednaConnError, "Connection is closed.");

#ifdef NON_BLOCKING
	// Non-blocking variant for >= 1.9.
	SQ q = { conn, STR2CSTR(query) };
	
	// Synchronize across threads using this instance and execute.
	res = rb_mutex_synchronize(rb_iv_get(self, IV_MUTEX), (void*)sedna_execute, (VALUE)&q);
#else
	// Blocking variant for < 1.9.
	res = SEexecute(conn, STR2CSTR(query));
#endif

	switch(res) {
		case SEDNA_QUERY_SUCCEEDED:
			// Return the results if this was a query.
			return sedna_get_results(conn);
		case SEDNA_UPDATE_SUCCEEDED:
		case SEDNA_BULK_LOAD_SUCCEEDED:
			// Return nil if this was an update or bulk load.
			return Qnil;
		default:
			// Raise an exception if something else happened.
			sedna_err(conn, res);
			return Qnil;
	}
}

/*
 * call-seq:
 *   sedna.load_document(document, doc_name, col_name = nil) -> nil
 *
 * Creates a new document named +doc_name+ in collection +col_name+, or as a
 * stand-alone document if +col_name+ is +nil+. The string +document+ is
 * subsequently loaded into the newly created document. As an alternative, the
 * argument +document+ may be an IO object (or any descendant, such as a File
 * object).
 *
 * If the document was successfully loaded, this method returns +nil+. If an
 * error occurs, a Sedna::Exception is raised.
 *
 * ==== Examples
 *
 * Create a new standalone document and retrieve it.
 *
 *   sedna.load_document "<my_document>Hello world!</my_document>", "my_doc"       
 *     #=> nil
 *   sedna.execute "doc('my_doc')"
 *     #=> ["<?xml version=\"1.0\" standalone=\"yes\"?><my_document>Hello world!</my_document>"]
 *
 * Open a file and import its contents into a new document in an existing collection.
 *
 *   File.open "document.xml" do |file|
 *     sedna.load_document file, "my_doc", "my_col"
 *   end
 */
static VALUE cSedna_load_document(int argc, VALUE *argv, VALUE self)
{
	int res = 0;
	SC *conn = sedna_struct(self);
	VALUE document, doc_name, col_name, buf;
	char *doc_name_c, *col_name_c;

	// Verify that the connection is OK.
	if(SEconnectionStatus(conn) != SEDNA_CONNECTION_OK) rb_raise(cSednaConnError, "Connection is closed.");

	// 2 mandatory arguments, 1 optional.
	rb_scan_args(argc, argv, "21", &document, &doc_name, &col_name);
	doc_name_c = STR2CSTR(doc_name);
	col_name_c = NIL_P(col_name) ? NULL : STR2CSTR(col_name);

	if(TYPE(document) == T_FILE) {
		// If the document is an IO object...
		while(!NIL_P(buf = rb_funcall(document, rb_intern("read"), 1, INT2NUM(LOAD_BUF_LEN)))) {
			// ...read from it until we reach EOF and load the data.
			res = SEloadData(conn, STR2CSTR(buf), RSTRING_LEN(buf), doc_name_c, col_name_c);
			VERIFY_RES(SEDNA_DATA_CHUNK_LOADED, res, conn);
		}

		// If there is no data, raise an exception.
		if(res == 0) rb_raise(cSednaException, "Document is empty.");
	} else {
		// If the document is not an IO object, verify it is a string instead.
		Check_Type(document, T_STRING);
		
		// If there is no data, raise an exception.
		if(RSTRING_LEN(document) == 0) rb_raise(cSednaException, "Document is empty.");

		// Load the data.
		res = SEloadData(conn, STR2CSTR(document), RSTRING_LEN(document), doc_name_c, col_name_c);
		VERIFY_RES(SEDNA_DATA_CHUNK_LOADED, res, conn);
	}

	// Signal that we're finished.
	res = SEendLoadData(conn);
	VERIFY_RES(SEDNA_BULK_LOAD_SUCCEEDED, res, conn);

	// Always return nil if successful.
	return Qnil;
}

/* :nodoc:
 *
 * Turn autocommit on or off.
 */
static VALUE cSedna_autocommit_set(VALUE self, VALUE auto_commit)
{
	int val = (RTEST(auto_commit) ? Qtrue : Qfalse);
	SC *conn = sedna_struct(self);

	// Switch autocommit mode on or off.
	SWITCH_SEDNA_AUTOCOMMIT(conn, val);
	
	// Set the instance variable accordingly so it can be re-set after a transaction.
	rb_iv_set(self, IV_AUTOCOMMIT, val);

	// Always return nil if successful.
	return Qnil;
}

/* :nodoc:
 *
 * Get the current autocommit value.
 */
static VALUE cSedna_autocommit_get(VALUE self)
{
	return rb_iv_get(self, IV_AUTOCOMMIT);
}

/*
 * call-seq:
 *   sedna.transaction { ... } -> nil
 *
 * Wraps the given block in a \transaction. If the block runs
 * completely, the \transaction is committed. If the stack is unwinded
 * prematurely, the \transaction is rolled back. This typically happens
 * when an \Exception is raised by calling +raise+ or a Symbol is thrown by
 * invoking +throw+. Note that Exceptions will not be rescued -- they will be
 * re-raised after rolling back the \transaction.
 *
 * This method returns +nil+ if the \transaction is successfully committed
 * to the database. If the given block completes successfully, but the
 * \transaction fails to be committed, a Sedna::TransactionError will
 * be raised. 
 *
 * Transactions cannot be nested or executed simultaneously with the same connection.
 * Calling this method inside a block that is passed to another transaction, or
 * with the same connection in two concurrent threads will raise a
 * Sedna::TransactionError on the second invocation.
 *
 * ==== Examples
 *
 *   sedna.transaction do
 *     count = sedna.execute "count(collection('my_col')/items)"  #=> 0
 *     sedna.execute "update insert <total>#{count}</total> into doc('my_doc')"
 *     # ...
 *   end
 *   # Transaction is committed.
 *
 *   sedna.transaction do
 *     count = sedna.execute "count(collection('my_col')/items)"  #=> 0
 *     throw :no_items if count == 0
 *     # ... never get here
 *   end
 *   # Transaction is rolled back.
 */
static VALUE cSedna_transaction(VALUE self)
{
	int status;
	SC *conn = sedna_struct(self);

	// Disable autocommit mode.
	SEDNA_AUTOCOMMIT_DISABLE(conn);
	
	// Begin the transaction.
	sedna_tr_begin(conn);

	// Yield to the given block and protect it so we can always commit or rollback.
	rb_protect(rb_yield, Qnil, &status);

	if(status == 0) {
		if(SEtransactionStatus(conn) == SEDNA_TRANSACTION_ACTIVE) {
			// Attempt to commit if block completed successfully.
			sedna_tr_commit(conn);

			// Turn autocommit back on if it was set.
			SWITCH_SEDNA_AUTOCOMMIT(conn, rb_iv_get(self, IV_AUTOCOMMIT));
		} else {
			// Turn autocommit back on if it was set.
			SWITCH_SEDNA_AUTOCOMMIT(conn, rb_iv_get(self, IV_AUTOCOMMIT));

			// If there is no current transaction, raise an error.
			rb_raise(cSednaTrnError, "The transaction was prematurely ended, but no error was encountered. Did you rescue an exception or reset the connection inside the transaction?");
		}
	} else {
		// Stack has unwinded, attempt to roll back!
		if(SEtransactionStatus(conn) == SEDNA_TRANSACTION_ACTIVE) sedna_tr_rollback(conn);

		// Turn autocommit back on if it was set.
		SWITCH_SEDNA_AUTOCOMMIT(conn, rb_iv_get(self, IV_AUTOCOMMIT));

		// Re-raise any exception or re-throw whatever was thrown.
		rb_jump_tag(status);
	}

	// Always return nil if successful.
	return Qnil;
}


// Initialize the extension ==============================================

void Init_sedna()
{
	/*
	 * Objects of class Sedna represent a connection to a \Sedna XML
	 * database. Establish a new connection by invoking the Sedna.connect
	 * class method.
	 *
	 *   connection_details = {
	 *     :database => "my_db",
	 *     :host => "localhost",
	 *     :username => "SYSTEM",
	 *     :password => "MANAGER",
	 *   }
	 *   Sedna.connect(connection_details) do |sedna|
	 *     # Query the database.
	 *     # The connection is closed automatically.
	 *   end
	 *
	 * See the README for a high-level overview of how to use this library.
	 */
	cSedna = rb_define_class("Sedna", rb_cObject);

	rb_define_alloc_func(cSedna, cSedna_s_new);
	rb_define_singleton_method(cSedna, "connect", cSedna_s_connect, 1);
	rb_define_singleton_method(cSedna, "version", cSedna_s_version, 0);
	rb_define_singleton_method(cSedna, "blocking?", cSedna_s_blocking, 0);

	rb_define_method(cSedna, "initialize", cSedna_initialize, 1);
	rb_define_method(cSedna, "connected?", cSedna_connected, 0);
	rb_define_method(cSedna, "close", cSedna_close, 0);
	rb_define_method(cSedna, "reset", cSedna_reset, 0);
	rb_define_method(cSedna, "transaction", cSedna_transaction, 0);
	rb_define_method(cSedna, "execute", cSedna_execute, 1);
	rb_define_undocumented_alias(cSedna, "query", "execute");
	rb_define_method(cSedna, "load_document", cSedna_load_document, -1);

	/*
	 * Document-attr: autocommit
	 *
	 * When autocommit is set to +true+ (default), database queries can be run
	 * without explicitly wrapping them in a transaction. Each query that is not
	 * part of a \transaction is automatically committed to the database.
	 * Explicit transactions in auto-commit mode will still be committed
	 * atomically.
	 * 
	 * When autocommit is set to +false+, queries can only be run inside an
	 * explicit transaction. Queries run outside transactions will fail with a
	 * Sedna::Exception.
	 */
	/* Trick RDoc into thinking this is a regular attribute. We documented the
	 * attribute above.
	rb_define_attr(cSedna, "autocommit", 1, 1);
	 */
	rb_define_method(cSedna, "autocommit=", cSedna_autocommit_set, 1);
	rb_define_method(cSedna, "autocommit", cSedna_autocommit_get, 0);

	/*
	 * The result of a database query is stored in a Sedna::Set object, which
	 * is a subclass of Array. Additional details about the executed query, such
	 * as timing and debug information, may be added to Sedna::Set objects in
	 * future versions of this library.
	 */
	// Stick to Array for result sets.
	//cSednaSet = rb_define_class_under(cSedna, "Set", rb_cArray);

	/*
	 * Generic exception class for errors. All errors raised by the \Sedna
	 * client library are of type Sedna::Exception.
	 *
	 * === Subclasses
	 *
	 * For some specific errors, an exception of a particular subtype is raised.
	 *
	 * [Sedna::AuthenticationError]
	 *   Raised when a database connection was successfully established, but
	 *   the supplied credentials were incorrect. Can only be raised when
	 *   invoking Sedna.connect.
	 * [Sedna::ConnectionError]
	 *   Raised when a connection to a database could not be established or when
	 *   a connection could not be closed. Can be raised when invoking Sedna.connect
	 *   or Sedna#close.
	 * [Sedna::TransactionError]
	 *   Raised when a transaction could not be committed.
	 */
	cSednaException = rb_define_class_under(cSedna, "Exception", rb_eStandardError);

	/*
	 * Sedna::AuthenticationError is a subclass of Sedna::Exception, and is
	 * raised when a database connection was successfully established, but the
	 * supplied credentials were incorrect. Can only be raised when invoking
	 * Sedna.connect.
	 */
	cSednaAuthError = rb_define_class_under(cSedna, "AuthenticationError", cSednaException);

	/*
	 * Sedna::ConnectionError is a subclass of Sedna::Exception, and is
	 * raised when a connection to a database could not be established or when
	 * a connection could not be closed. Can be raised when invoking Sedna.connect
	 * or Sedna#close.
	 */
	cSednaConnError = rb_define_class_under(cSedna, "ConnectionError", cSednaException);

	/*
	 * Sedna::TransactionError is a subclass of Sedna::Exception, and is
	 * raised when a transaction could not be committed.
	 */
	cSednaTrnError = rb_define_class_under(cSedna, "TransactionError", cSednaException);
}