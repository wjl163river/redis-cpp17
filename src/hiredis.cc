#include "hiredis.h"
RedisReply::RedisReply()
:str(nullptr)
{

}

RedisReply::~RedisReply()
{
	if (str != nullptr)
	{
		zfree(str);
	}
}

RedisReader::RedisReader(Buffer *buffer)
:buffer(buffer),
 pos(0),
 err(0),
 ridx(-1)
{
	errstr[0] = '\0';
}

RedisReader::RedisReader()
:pos(0),
 err(0),
 ridx(-1)
{
	buffer = &buf;
	errstr[0] = '\0';
}

void RedisReader::redisReaderSetError(int32_t type,const char *str)
{
	size_t len;
	/* Clear input buffer on errors. */
	if (buffer != nullptr)
	{
		buffer->retrieveAll();
		pos = 0;
	}
	/* Reset task stack. */
	ridx = -1;
	/* Set error. */
	err = type;
	len = strlen(str);
	len = len < (sizeof(errstr) - 1) ? len : (sizeof(errstr) - 1);
	memcpy(errstr,str,len);
	errstr[len] = '\0';
}

static size_t chrtos(char *buffer,size_t size,char byte)
{
    size_t len = 0;
    switch(byte)
    {
		case '\\':
		case '"': { len = snprintf(buffer,size,"\"\\%c\"",byte); break; }
		case '\n': len = snprintf(buffer,size,"\"\\n\""); break;
		case '\r': len = snprintf(buffer,size,"\"\\r\""); break;
		case '\t': len = snprintf(buffer,size,"\"\\t\""); break;
		case '\a': len = snprintf(buffer,size,"\"\\a\""); break;
		case '\b': len = snprintf(buffer,size,"\"\\b\""); break;
		default:
			if (isprint(byte))
			{
				len = snprintf(buffer,size,"\"%c\"",byte);
			}
			else
			{
				len = snprintf(buffer,size,"\"\\x%02x\"",(unsigned char)byte);
			}
			break;
    }
    return len;
}

void RedisReader::redisReaderSetErrorProtocolByte(char byte)
{
    char cbuf[8], sbuf[128];
    chrtos(cbuf,sizeof(cbuf),byte);
    snprintf(sbuf,sizeof(sbuf),"Protocol error, got %s as reply type byte",cbuf);
    redisReaderSetError(REDIS_ERR_PROTOCOL,sbuf);
}

static int32_t intlen(int32_t i)
{
	int32_t len = 0;
	if (i < 0)
	{
		len++;
		i = -i;
	}

	do
	{
		len++;
		i /= 10;
	}while(i);
	return len;
}

/* Create a reply object */
RedisReplyPtr createReplyObject(int32_t type)
{
	RedisReplyPtr r(new RedisReply());
	if (r == nullptr)
	{
		return nullptr;
	}

	r->type = type;
	return r;
}

int64_t RedisReader::readLongLong(const char *s)
{
	int64_t v = 0;
	int32_t dec,mult = 1;
	char c;

	if (*s == '-')
	{
		mult = -1;
		s++;
	}
	else if (*s == '+')
	{
		mult = 1;
		s++;
	}

	while ((c = * (s++)) != '\r')
	{
		dec = c - '0';

		if (dec >= 0 && dec < 10)
		{
			v *= 10;
			v += dec;
		}
		else 
		{ 
			/* Should not happen... */ 
			return - 1;
		}
	}
	return mult * v;
}

/* Find pointer to \r\n. */
static const char *seekNewline(const char *s,size_t len)
{
	int32_t pos = 0;
	int32_t _len = len - 1;
	/* Position should be < len-1 because the character at "pos" should be
	 * followed by a \n. Note that strchr cannot be used because it doesn't
	 * allow to search a limited length and the buffer that is being searched
	 * might not have a trailing nullptr character. */
	while (pos < _len)
	{
		while (pos < _len && s[pos] != '\r')
		{
			pos++;
		}

		if (s[pos] != '\r')
		{ /* Not found. */
			return nullptr;
		}
		else
		{
			if (s[pos + 1] == '\n')
			{
				return s + pos;
			}
			else
			{
				/* Continue searching. */
				pos++;
			}
		}
	}
	return nullptr;
}

const char *RedisReader::readLine(int32_t * _len)
{
	const char *p;
	const char *s;
	int32_t len;
	p = buffer->peek() + pos;
	s = seekNewline(p,(buffer->readableBytes() - pos));
	if (s != nullptr)
	{
		len = s - (buffer->peek() + pos);
		pos += len + 2; 			/* skip \r\n */

		if (_len)
			*_len = len;
		return p;
	}
	return nullptr;
}

const char *RedisReader::readBytes(uint32_t bytes)
{
	const char *p;
	if (buffer->readableBytes() - pos >= bytes)
	{
		p = buffer->peek() + pos;
		pos += bytes;
		return p;
	}
	return nullptr;
}

RedisReplyPtr createString(const RedisReadTask *task,const char *str,size_t len)
{
	RedisReplyPtr r,parent;
	r = createReplyObject(task->type);
	if (r == nullptr)
	{
		return nullptr;
	}

	char *buffer = (char*)zmalloc(len + 1);
	if (buffer == nullptr)
	{
		return nullptr;
	}

	assert(task->type == REDIS_REPLY_ERROR ||
			task->type == REDIS_REPLY_STATUS ||
			task->type == REDIS_REPLY_STRING);

	/* Copy string value */
	memcpy(buffer,str,len);
	buffer[len] = '\0';
	r->str = buffer;
	r->len = len;

	if (task->parent)
	{
		parent = task->parent->obj.lock();
		assert(parent != nullptr);
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element.push_back(r);
	}
	return r;
}

RedisReplyPtr createArray(const RedisReadTask *task,int32_t elements)
{
	RedisReplyPtr r,parent;
	r = createReplyObject(REDIS_REPLY_ARRAY);
	if (r == nullptr) 
	{
		return nullptr; 
	}

	if (elements > 0)
	{
		r->element.reserve(elements);
	}

	r->elements = elements;
	if (task->parent)
	{
		parent = task->parent->obj.lock();
		assert(parent != nullptr);
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element.push_back(r);
	}
	return r;
}

RedisReplyPtr createInteger(const RedisReadTask *task,int64_t value)
{
	RedisReplyPtr r,parent;
	r = createReplyObject(REDIS_REPLY_INTEGER);
	if (r == nullptr)
	{
		return nullptr;
	}

	r->integer = value;
	if (task->parent)
	{
		parent = task->parent->obj.lock();
		assert(parent != nullptr);
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element.push_back(r);
	}
	return r;
}

RedisReplyPtr createNil(const RedisReadTask *task)
{
	RedisReplyPtr r,parent;
	r = createReplyObject(REDIS_REPLY_NIL);
	if (r == nullptr)
	{
		return nullptr;
	}

	if (task->parent)
	{
		parent = task->parent->obj.lock();
		assert(parent != nullptr);
		assert(parent->type == REDIS_REPLY_ARRAY);
		parent->element.push_back(r);
	}
	return r;
}

/* Helper that calculates the bulk length given a certain string length. */
static size_t bulklen(size_t len)
{
	return 1 + intlen(len) + 2 + len + 2;
}

static char *nextArgument(char *start,char **str,size_t *len)
{
    char *p = start;
    if (p[0] != '$')
    {
        p = strchr(p,'$');
        if (p == nullptr)
		{
        	return nullptr;
		}
    }

    *len = (int32_t)strtol(p+1,nullptr,10);
    p = strchr(p,'\r');
    assert(p);
    *str = p+2;
    return p+2+(*len)+2;
}

void RedisContext::redisSetError(int32_t type,const char *str)
{
	size_t	len;
	err = type;
	if (str != nullptr)
	{
		len = strlen(str);
		len = len < (sizeof(errstr) - 1) ? len: (sizeof(errstr) - 1);
		memcpy(errstr, str, len);
		errstr[len] = '\0';
	}
	else
	{
		assert(type == REDIS_ERR_IO);
		memcpy(errstr, strerror(errno),sizeof(errstr));
	}
}

void RedisReader::moveToNextTask()
{
	RedisReadTask *cur,*prv;
	while (ridx >= 0)
	{
		/* Return a.s.a.p. when the stack is now empty. */
		if (ridx == 0)
		{
			ridx--;
			return;
		}

		cur = & (rstack[ridx]);
		prv = & (rstack[ridx - 1]);
		assert(prv->type == REDIS_REPLY_ARRAY);

		if (cur->idx == prv->elements - 1)
		{
			ridx--;
		}
		else
		{
			/* Reset the type because the next item can be anything */
			assert(cur->idx < prv->elements);
			cur->type = -1;
			cur->elements = -1;
			cur->idx++;
			return;
		}
	}
}

int32_t RedisReader::processLineItem()
{
	RedisReadTask *cur = &(rstack[ridx]);
	RedisReplyPtr obj;
	const char *p;
	int32_t len;

	if ((p = readLine(&len)) != nullptr)
	{
		if (cur->type == REDIS_REPLY_INTEGER)
		{
			if (fn.createIntegerFuc)
			{
				obj = fn.createIntegerFuc(cur,readLongLong(p));
			}
			else
			{
				assert(false);
			}
		}
		else
		{
			/* Type will be error or status. */
			if (fn.createStringFuc)
			{
				obj = fn.createStringFuc(cur,p,len);
			}
			else
			{
				assert(false);
			}
		}

		if (obj == nullptr)
		{
			redisReaderSetErrorOOM();
			return REDIS_ERR;
		}

		/* Set reply if this is the root object. */
		if (ridx == 0)
		{
			reply = obj;
		}

		moveToNextTask();
		return REDIS_OK;
	}
	return REDIS_ERR;
}

int32_t RedisReader::processBulkItem()
{
	RedisReadTask *cur = &(rstack[ridx]);
	RedisReplyPtr obj;
	const char *p;
	const char *s;
	int32_t	len;
	uint32_t bytelen;
	int32_t success = 0;

	p = buffer->peek() + pos;
	s = seekNewline(p,buffer->readableBytes() - pos);

	if (s != nullptr)
	{
		p = buffer->peek() + pos;
		bytelen = s - (buffer->peek() + pos) + 2; /* include \r\n */
		len = readLongLong(p);

		if (len < 0)
		{
			/* The nil object can always be created. */
			if (fn.createNilFuc)
			{
				obj = fn.createNilFuc(cur);
			}
			else
			{
				assert(false);
			}
			success = 1;
		}
		else
		{
			/* Only continue when the buffer contains the entire bulk item. */
			bytelen += len + 2; 		/* include \r\n */
			if (pos + bytelen <= buffer->readableBytes())
			{
				if (fn.createStringFuc)
				{
					obj = fn.createStringFuc(cur,s + 2,len);
				}
				else
				{
					assert(false);
				}
				success  = 1;
			}
		}

		/* Proceed when obj was created. */
		if (success)
		{
			if (obj == nullptr)
			{
				redisReaderSetErrorOOM();
				return REDIS_ERR;
			}

			pos += bytelen;
			/* Set reply if this is the root object. */
			if (ridx == 0)
			{
				reply = obj;
			}

			moveToNextTask();
			return REDIS_OK;
		}
	}
	return REDIS_ERR;
}

int32_t RedisReader::processMultiBulkItem()
{
	RedisReadTask *cur = &(rstack[ridx]);
	RedisReplyPtr obj;
	const char *p;
	int32_t elements;
	int32_t  root = 0;

	/* Set error for nested multi bulks with depth > 7 */
	if (ridx == 8)
	{
		redisReaderSetError(REDIS_ERR_PROTOCOL,"No support for nested multi bulk replies with depth > 7");
		return REDIS_ERR;
	}

	if ((p = readLine(nullptr)) != nullptr)
	{
		elements = readLongLong(p);
		root = (ridx == 0);
		if (elements == -1)
		{
			if (fn.createNilFuc)
			{
				obj = fn.createNilFuc(cur);
			}
			else
			{
				assert(false);
			}

			if (obj == nullptr)
			{
				redisReaderSetErrorOOM();
				return REDIS_ERR;
			}
			moveToNextTask();
		}
		else
		{
			if (fn.createArrayFuc)
			{
				obj = fn.createArrayFuc(cur,elements);
			}
			else
			{
				assert(false);
			}

			if (obj == nullptr)
			{
				redisReaderSetErrorOOM();
				return REDIS_ERR;
			}

			/* Modify task stack when there are more than 0 elements. */
			if (elements > 0)
			{
				cur->elements = elements;
				cur->obj = obj;
				ridx++;
				rstack[ridx].type = -1;
				rstack[ridx].elements = -1;
				rstack[ridx].idx = 0;
				rstack[ridx].parent = cur;
				rstack[ridx].privdata = privdata;
			}
			else
			{
				moveToNextTask();
			}
		}

		/* Set reply if this is the root object. */
		if (root)
		{
			reply = obj;
		}
		return REDIS_OK;
	}
	return REDIS_ERR;
}

int32_t RedisReader::processItem()
{
	RedisReadTask *cur = &(rstack[ridx]);
	const char *p;

/* check if we need to read type */
	if (cur->type < 0)
	{
		if ((p = readBytes(1)) != nullptr)
		{
			switch (p[0])
			{
				case '-':
					cur->type = REDIS_REPLY_ERROR;
					break;
				case '+':
					cur->type = REDIS_REPLY_STATUS;
					break;
				case ':':
					cur->type = REDIS_REPLY_INTEGER;
					break;
				case '$':
					cur->type = REDIS_REPLY_STRING;
					break;
				case '*':
					cur->type = REDIS_REPLY_ARRAY;
					break;
				default:
					/* could not consume 1 byte */
					redisReaderSetErrorProtocolByte(*((char *)p));
				return REDIS_ERR;
			}
		}
		else
		{
			return REDIS_ERR;
		}
	}

/* process typed item */
	switch (cur->type)
	{
		case REDIS_REPLY_ERROR:
		case REDIS_REPLY_STATUS:
		case REDIS_REPLY_INTEGER:
			return processLineItem();
		case REDIS_REPLY_STRING:
			return processBulkItem();
		case REDIS_REPLY_ARRAY:
			return processMultiBulkItem();
		default:
			return REDIS_ERR; /* Avoid warning. */
	}
}

void RedisReader::redisReaderSetErrorOOM()
{
	redisReaderSetError(REDIS_ERR_OOM,"Out of memory");
}

int32_t RedisReader::redisReaderGetReply(RedisReplyPtr &reply)
{
	if (err)
	{
		return REDIS_ERR;
	}

	if (buffer->readableBytes() == 0)
	{
		return REDIS_OK;
	}

	if (ridx == -1)
	{
		rstack[0].type = -1;
		rstack[0].elements = -1;
		rstack[0].idx = -1;
		rstack[0].parent = nullptr;
		rstack[0].privdata = privdata;
		ridx = 0;
	}

	while (ridx >= 0)
	{
		if (processItem() != REDIS_OK)
		{
			break;
		}
	}

	if (err)
	{
		return REDIS_ERR;
	}

	if (pos >= 1024)
	{
		buffer->retrieve(pos);
		pos = 0;
	}

	/* Emit a reply when there is one. */
	if (ridx == -1)
	{
		reply = this->reply;
		this->reply.reset();
	}
	return REDIS_OK;
}

RedisAsyncCallback::RedisAsyncCallback()
:data(nullptr),
 len(0)
{

}

RedisAsyncCallback::~RedisAsyncCallback()
{
	if(data != nullptr)
	{
		zfree(data);
	}
}

RedisContext::RedisContext()
:reader(new RedisReader())
{
	clear();
}

RedisContext::RedisContext(Buffer *buffer,int32_t sockfd)
:reader(new RedisReader(buffer)),
 fd(sockfd)
{
	clear();
}

void RedisContext::clear()
{
	flags &= ~REDIS_BLOCK;
	err = 0;
	errstr[0] = '\0';
	fd = 0;
}

RedisContext::~RedisContext()
{

}

void RedisContext::setBlock()
{
	flags |= REDIS_BLOCK;
}

void RedisContext::setDisConnected()
{
	flags &= REDIS_CONNECTED;
}

void RedisContext::setConnected()
{
	flags != REDIS_CONNECTED;
}

/* Internal helper function to try and get a reply from the reader,
 * or set an error in the contextPtr otherwise. */

int32_t RedisContext::redisGetReplyFromReader(RedisReplyPtr &reply)
{
	if (reader->redisReaderGetReply(reply) == REDIS_ERR)
	{
		redisSetError(reader->err,reader->errstr);
		return REDIS_ERR;
	}
	return REDIS_OK;
}

/* Write the output buffer to the socket.
 *
 * Returns REDIS_OK when the buffer is empty, or (a part of) the buffer was
 * successfully written to the socket. When the buffer is empty after the
 * write operation, "done" is set to 1 (if given).
 *
 * Returns REDIS_ERR if an error occurred trying to write and sets
 * c->errstr to hold the appropriate error string.
 */

int32_t RedisContext::redisBufferWrite(int32_t *done)
{
	int32_t nwritten;
	if (err) { return REDIS_ERR; }

	if (sender.readableBytes() > 0)
	{
		nwritten = ::write(fd,sender.peek(),sender.readableBytes());
		if (nwritten < 0 )
		{
			if ((errno == EAGAIN && ! (flags & REDIS_BLOCK)) || (errno == EINTR))
			{
				/* Try again later */
			}
			else
            {
				redisSetError(REDIS_ERR_IO,nullptr);
				return REDIS_ERR;
			}
		}
		else if (nwritten > 0)
		{
			if (nwritten == sender.readableBytes())
			{
			    sender.retrieveAll();
			}
			else
			{
			    sender.retrieve(nwritten);
			}
		}
	}

	if (done != nullptr)
	{
	    *done = (sender.readableBytes() == 0);
	}
	return REDIS_OK;
}

/* Use this function to handle a read event on the descriptor. It will try
 * and read some bytes from the socket and feed them to the reply parser.
 *
 * After this function is called, you may use redisContextReadReply to
 * see if there is a reply available. */

int32_t RedisContext::redisBufferRead()
{
	int32_t savedErrno = 0;
    /* Return early when the contextPtr has seen an error. */
	if(err)
	{
		return REDIS_ERR;
	}

	ssize_t n = reader->buffer->readFd(fd,&savedErrno);
	if (n > 0)
	{
		
	}
	else if (n == 0)
	{
		redisSetError(REDIS_ERR_EOF,"Server closed the connection");
		return REDIS_ERR;
	}
	else
	{
		if ((errno == EAGAIN && ! (flags & REDIS_BLOCK)) || (errno == EINTR))
		{
            /* Try again later */
		}
		else
		{
			redisSetError(REDIS_ERR_IO,nullptr);
			return REDIS_ERR;
		}
	}
	return REDIS_OK;
}

int32_t RedisContext::redisGetReply(RedisReplyPtr &reply)
{
	int32_t wdone = 0;
	RedisReplyPtr aux;
    /* Try to read pending replies */
	if (redisGetReplyFromReader(aux) == REDIS_ERR)
    {
		redisSetError(reader->err,reader->errstr);
		return REDIS_ERR;
	}

    /* For the blocking contextPtr, flush output buffer and read reply */
	if (aux == nullptr && flags & REDIS_BLOCK)
	{
        /* Write until done */
		do
		{
			if (redisBufferWrite(&wdone) == REDIS_ERR)
			{
				return REDIS_ERR;
			}

		}while(!wdone);


        /* Read until there is a reply */
		do
		{
			if (redisBufferRead() == REDIS_ERR)
			{
				return REDIS_ERR;
			}

			if (redisGetReplyFromReader(aux) == REDIS_ERR)
			{
				return REDIS_ERR;
			}
		}while(aux == nullptr);
	}

	reply = aux;
	return REDIS_OK;
}

int32_t RedisContext::redisAppendCommand(const char *format, ...)
{
    va_list ap;
    int32_t ret;

    va_start(ap,format);
    ret = redisvAppendCommand(format,ap);
    va_end(ap);
    return ret;
}

TcpConnectionPtr RedisAsyncContext::getServerConn()
{
	return serverConn;
}

std::mutex &RedisAsyncContext::getMutex()
{
	return mtx;
}

RedisContextPtr RedisAsyncContext::getRedisContext()
{
	return contextPtr;
}

void RedisAsyncContext::__redisAsyncCommand(const RedisCallbackFn &fn,
		const std::any &privdata,char *cmd,size_t len)
{
	RedisCallback cb;
	cb.fn = std::move(fn);
	cb.privdata = privdata;

	RedisAsyncCallbackPtr asyncCallback(new RedisAsyncCallback());
	asyncCallback->data = cmd;
	asyncCallback->len = len;
	asyncCallback->cb = std::move(cb);
	{
		std::unique_lock<std::mutex> lk(mtx);
		asyncCb.push_back(asyncCallback);
	}
	serverConn->sendPipe(cmd,len);
}

int32_t redisvFormatCommand(char **target,const char *format,va_list ap)
{
	const char *c = format;
	char *cmd = nullptr; 					/* final command */
	int32_t pos;							/* position in final command */
	sds curarg, newarg; 				/* current argument */
	int32_t touched = 0;					/* was the current argument touched? */
	char **curargv = nullptr;
	char **newargv = nullptr;
	int32_t argc = 0;
	int32_t totlen = 0;
	int32_t j;

	/* Abort if there is not target to set */
	if (target == nullptr)
	{
		return -1;
	}

	/* Build the command string accordingly to protocol */
	curarg = sdsempty();
	if (curarg == nullptr)
	{
		return -1;
	}

	while (*c != '\0')
	{
		if (*c != '%' || c[1] == '\0')
		{
			if (*c == ' ')
			{
				if (touched)
				{
					newargv = (char**)zrealloc(curargv,sizeof(char *) * (argc + 1));
					if (newargv == nullptr)
					{
						goto err;
					}

					curargv = newargv;
					curargv[argc++] = curarg;
					totlen += bulklen(sdslen(curarg));
					/* curarg is put in argv so it can be overwritten. */
					curarg = sdsempty();
					if (curarg == nullptr)
					{
						goto err;
					}
					touched  = 0;
				}
			}
			else
			{
				newarg = sdscatlen(curarg,c,1);
				if (newarg == nullptr)
				{
					goto err;
				}

				curarg = newarg;
				touched = 1;
			}
		}
		else
		{
			char *arg;
			size_t size;
			/* Set newarg so it can be checked even if it is not touched. */
			newarg = curarg;
			switch (c[1])
			{
				case 's':
					arg = va_arg(ap,char *);
					size = strlen(arg);
					if (size > 0)
					{
						newarg = sdscatlen(curarg,arg,size);
					}
					break;
				case 'b':
					arg = va_arg(ap,char *);
					size = va_arg(ap,size_t);
					if (size > 0)
					{
						newarg = sdscatlen(curarg,arg,size);
					}
					break;
				case '%':
					newarg = sdscat(curarg,"%");
					break;
				default:
				/* Try to detect printf format */
				{
					static const char intfmts[] = "diouxX";
					char _format[16];
					const char *_p = c + 1;
					size_t _l = 0;
					va_list _cpy;

					/* Flags */
					if (*_p != '\0' && *_p == '#')
						_p ++;

					if (*_p != '\0' && *_p == '0')
						_p ++;

					if (*_p != '\0' && *_p == '-')
						_p ++;

					if (*_p != '\0' && *_p == ' ')
						_p ++;

					if (*_p != '\0' && *_p == '+')
						_p ++;

					/* Field width */
					while (*_p != '\0' && isdigit(*_p))
					_p ++;

					/* Precision */
					if (*_p == '.')
					{
						_p ++;
						while (*_p != '\0' && isdigit(*_p))
							_p ++;
					}
					/* Copy va_list before consuming with va_arg */
					va_copy(_cpy,ap);
					/* Integer conversion (without modifiers) */
					if (strchr(intfmts,*_p) != nullptr)
					{
						va_arg(ap,int32_t);
						goto fmt_valid;
					}
					/* Double conversion (without modifiers) */
					if (strchr("eEfFgGaA",*_p) != nullptr)
					{
						va_arg(ap,double);
						goto fmt_valid;
					}
					/* Size: char */
					if (_p[0] == 'h' && _p[1] == 'h')
					{
						_p += 2;
						if (*_p != '\0' && strchr(intfmts,*_p) != nullptr)
						{
							va_arg(ap,int32_t);	/* char gets promoted to int32_t */
							goto fmt_valid;
						}
						goto fmt_invalid;
					}
					/* Size: short */
					if (_p[0] == 'h')
					{
						_p	+= 1;
						if (*_p != '\0' && strchr(intfmts,*_p) != nullptr)
						{
							va_arg(ap,int32_t);	/* short gets promoted to int32_t */
							goto fmt_valid;
						}
						goto fmt_invalid;
					}
					/* Size: long long */
					if (_p[0] == 'l' && _p[1] == 'l')
					{
						_p	+= 2;
						if (*_p != '\0' && strchr(intfmts,*_p) != nullptr)
						{
							va_arg(ap,long long);
							goto fmt_valid;
						}

						goto fmt_invalid;
					}
					/* Size: long */
					if (_p[0] == 'l')
					{
						_p += 1;
						if (*_p != '\0' && strchr(intfmts,*_p) != nullptr)
						{
							va_arg(ap, long);
							goto fmt_valid;
						}
						goto fmt_invalid;
					}
	fmt_invalid:
					va_end(_cpy);
					goto err;
	fmt_valid:
					_l = (_p +1) -c;
					if (_l < sizeof(_format) - 2)
					{
						memcpy(_format, c, _l);
						_format[_l] = '\0';
						newarg = sdscatvprintf(curarg, _format, _cpy);

						/* Update current position (note: outer blocks
						 * increment c twice so compensate here) */
						c = _p -1;
					}
					va_end(_cpy);
					break;
				
				}
			}
			
			if (newarg == nullptr)
			{
				goto err;
			}

			curarg = newarg;
			touched = 1;
			c++;
		}
		c++;
	}
	/* Add the last argument if needed */
	if (touched)
	{
		newargv = (char **)zrealloc(curargv, sizeof(char *) * (argc + 1));
		if (newargv == nullptr)
		{
			goto err;
		}

		curargv = newargv;
		curargv[argc++] = curarg;
		totlen += bulklen(sdslen(curarg));
	}
	else 
	{ 
		sdsfree(curarg); 
	}

	/* Clear curarg because it was put in curargv or was free'd. */
	curarg = nullptr;
	/* Add bytes needed to hold multi bulk count */
	totlen += 1 + intlen(argc) + 2;
	/* Build the command at protocol level */
	cmd = (char *)zmalloc(totlen + 1);
	if (cmd == nullptr)
	{
		goto err; 
	}

	pos = sprintf(cmd,"*%d\r\n",argc);
	for (j = 0; j < argc; j++)
	{
		pos += sprintf(cmd + pos,"$%zu\r\n",sdslen(curargv[j]));
		memcpy(cmd + pos, curargv[j], sdslen(curargv[j]));
		pos += sdslen(curargv[j]);
		sdsfree(curargv[j]);
		cmd[pos++]	= '\r';
		cmd[pos++]	= '\n';
	}

	assert(pos == totlen);
	cmd[pos] = '\0';
	zfree(curargv);
	*target = cmd;
	return totlen;
err:
	while (argc--)
	{
		sdsfree(curargv[argc]);
	}

	zfree(curargv);
	if (curarg != nullptr)
	{
		sdsfree(curarg);
	}

	if (cmd != nullptr)
	{
		zfree(cmd);
	}
	return -1;
}

/* Format a command according to the Redis protocol. This function
 * takes a format similar to printf:
 *
 * %s represents a C null terminated string you want to interpolate
 * %b represents a binary safe string
 *
 * When using %b you need to provide both the pointer to the string
 * and the length in bytes as a size_t. Examples:
 *
 * len = redisFormatCommand(target, "GET %s", mykey);
 * len = redisFormatCommand(target, "SET %s %b", mykey, myval, myvallen);
 */

int32_t redisFormatCommand(char **target,const char *format, ...)
{
    va_list ap;
    int32_t len;
    va_start(ap,format);
    len = redisvFormatCommand(target,format,ap);
    va_end(ap);
    if (len < 0)
    {
    	len = -1;
    }
    return len;
}

RedisAsyncContext::RedisAsyncContext(Buffer *buffer,const TcpConnectionPtr &conn)
:contextPtr(new RedisContext(buffer,conn->getSockfd())),
 serverConn(conn),
 err(0),
 errstr(nullptr),
 data(nullptr)
{
	contextPtr->setConnected();
}

RedisAsyncContext::~RedisAsyncContext()
{

}

int RedisAsyncContext::redisvAsyncCommand(const RedisCallbackFn &fn,
		const std::any &privdata,const char *format, va_list ap)
{
	char *cmd;
	int32_t len;
	len = redisvFormatCommand(&cmd,format,ap);
	if (len < 0)
	{
		return REDIS_ERR;
	}

	__redisAsyncCommand(fn,privdata,cmd,len);
	return REDIS_OK;
}

int32_t RedisAsyncContext::redisGetReply(RedisReplyPtr &reply)
{
	return contextPtr->redisGetReply(reply);
}

int RedisAsyncContext::redisAsyncCommand(const RedisCallbackFn &fn,
		const std::any &privdata,const char *format, ...)
{
	va_list ap;
	va_start(ap,format);
	int status = redisvAsyncCommand(fn,privdata,format,ap);
	va_end(ap);
	return status;
}

RedisReplyPtr RedisContext::redisBlockForReply()
{
	RedisReplyPtr reply;
	if (flags & REDIS_BLOCK)
	{
		if (redisGetReply(reply) != REDIS_OK)
		{
			return nullptr;
		}
		return reply;
	}
	return nullptr;
}

int32_t RedisContext::redisvAppendCommand(const char *format,va_list ap)
{
	char *cmd;
	int32_t len;

	len = redisvFormatCommand(&cmd,format,ap);
	if (len == -1)
	{
		redisSetError(REDIS_ERR_OOM,"Out of memory");
		return REDIS_ERR;
	}
	else if (len == -2)
	{	
		redisSetError(REDIS_ERR_OTHER,"Invalid format string");
       	return REDIS_ERR;
	}

	redisAppendCommand(cmd,len);
	zfree(cmd);
	return REDIS_OK;
}

RedisReplyPtr RedisContext::redisCommand(const char *format, ...)
{
	va_list ap;
	RedisReplyPtr reply = nullptr;
	va_start(ap,format);
	reply = redisvCommand(format,ap);
	va_end(ap);
	return reply;
}

RedisReplyPtr RedisContext::redisvCommand(const char *format,va_list ap)
{
	if (redisvAppendCommand(format,ap) != REDIS_OK)
	{
		return nullptr;
	}
	return redisBlockForReply();
}

/* Format a command according to the Redis protocol. This function takes the
 * number of arguments, an array with arguments and an array with their
 * lengths. If the latter is set to NULL, strlen will be used to compute the
 * argument lengths.
 */

int32_t redisFormatCommandArgv(char **target,int32_t argc,
		const char **argv,const size_t *argvlen)
{
	char *cmd = nullptr; 					/* final command */
	int32_t pos;							/* position in final command */
	size_t	len;
	int32_t totlen,j;
	/* Calculate number of bytes needed for the command */
	totlen = 1 + intlen(argc) + 2;

	for (j = 0; j < argc; j++)
	{
		len = argvlen ? argvlen[j]: strlen(argv[j]);
		totlen += bulklen(len);
	}
	/* Build the command at protocol level */
	cmd = (char*)zmalloc(totlen + 1);
	if (cmd == nullptr)
	{
		return - 1;
	}

	pos = sprintf(cmd,"*%d\r\n",argc);

	for (j = 0; j < argc; j++)
	{
		len = argvlen ? argvlen[j]: strlen(argv[j]);
		pos += sprintf(cmd + pos,"$%zu\r\n",len);
		memcpy(cmd + pos, argv[j], len);
		pos += len;
		cmd[pos++] = '\r';
		cmd[pos++] = '\n';
	}

	assert(pos == totlen);
	cmd[pos] = '\0';
	*target = cmd;
	return totlen;
}

/* Return the number of digits of 'v' when converted to string in radix 10.
 * Implementation borrowed from link in redis/src/util.c:string2ll(). */
static uint32_t countDigits(uint64_t v)
{
	uint32_t result = 1;
	for (;;)
	{
		if (v < 10) return result;
		if (v < 100) return result + 1;
		if (v < 1000) return result + 2;
		if (v < 10000) return result + 3;
		v /= 10000U;
		result += 4;
	}
}

/* Format a command according to the Redis protocol using an sds string and
 * sdscatfmt for the processing of arguments. This function takes the
 * number of arguments, an array with arguments and an array with their
 * lengths. If the latter is set to NULL, strlen will be used to compute the
 * argument lengths.
 */
int32_t redisFormatSdsCommandArgv(sds *target,int32_t argc,
		const char **argv,const size_t *argvlen)
{
    sds cmd;
    uint64_t totlen;
    int32_t j;
    size_t len;

    /* Abort on a nullptr target */
    if (target == nullptr)
    {
    	return -1;
    }

    /* Calculate our total size */
    totlen = 1 + countDigits(argc)+2;
    for (j = 0; j < argc; j++)
    {
        len = argvlen ? argvlen[j] : strlen(argv[j]);
        totlen += bulklen(len);
    }
    /* Use an SDS string for command construction */
    cmd = sdsempty();
    if (cmd == nullptr)
    { return -1; }
    /* We already know how much storage we need */
    cmd = sdsMakeRoomFor(cmd, totlen);
    if (cmd == nullptr)
    {
    	return -1;
    }

    /* Construct command */
    cmd = sdscatfmt(cmd, "*%i\r\n", argc);
    for (j=0; j < argc; j++)
    {
        len = argvlen ? argvlen[j] : strlen(argv[j]);
        cmd = sdscatfmt(cmd, "$%u\r\n", len);
        cmd = sdscatlen(cmd, argv[j], len);
        cmd = sdscatlen(cmd, "\r\n", sizeof("\r\n")-1);
    }

    assert(sdslen(cmd)==totlen);
    *target = cmd;
    return totlen;
}

void RedisContext::redisAppendCommand(const char *cmd,size_t len)
{
	sender.append(cmd,len);
}

void RedisContext::redisAppendFormattedCommand(const char *cmd,size_t len)
{
	redisAppendCommand(cmd,len);
}

int32_t RedisContext::redisAppendCommandArgv(int32_t argc,
		const char **argv,const size_t *argvlen)
{
	char *cmd;
	int32_t len;
	len = redisFormatCommandArgv(&cmd,argc,argv,argvlen);

	if (len == -1)
	{
        redisSetError(REDIS_ERR_OOM,"Out of memory");
		return REDIS_ERR;
	}

	redisAppendCommand(cmd,len);
    zfree(cmd);
	return REDIS_OK;
}

RedisReplyPtr RedisContext::redisCommandArgv(int32_t argc,
		const char **argv,const size_t *argvlen)
{
	if (redisAppendCommandArgv(argc,argv,argvlen) != REDIS_OK)
	{ 
		return nullptr; 
	}
	return redisBlockForReply();
}

int32_t RedisContext::redisCheckSocketError()
{
	int32_t err = 0;
	socklen_t errlen = sizeof(err);

	if (::getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&errlen) == -1)
	{
	    redisSetError(REDIS_ERR_IO,"getsockopt(SO_ERROR)");
	    return REDIS_ERR;
	}

	if (err)
	{
	    errno = err;
	    redisSetError(REDIS_ERR_IO,nullptr);
	    return REDIS_ERR;
	}
	return REDIS_OK;
}

#define __MAX_MSEC (((LONG_MAX) - 999) / 1000)

int32_t RedisContext::redisContextWaitReady(int32_t msec)
{
	struct pollfd wfd[1];

	wfd[0].fd = fd;
	wfd[0].events = POLLOUT;

	if (errno == EINPROGRESS) 
	{
		int32_t res;
		if ((res = ::poll(wfd,1,msec) )== -1)
		{
			redisSetError(REDIS_ERR_IO, "poll(2)");
			return REDIS_ERR;
		}
		else if (res == 0)
		{
			errno = ETIMEDOUT;
			redisSetError(REDIS_ERR_IO,nullptr);
			return REDIS_ERR;
		}

		 if (redisCheckSocketError() !=  REDIS_OK)
		 {
			 return REDIS_ERR;
		 }
		 return REDIS_OK;
	}

	redisSetError(REDIS_ERR_IO,nullptr);
	return REDIS_ERR;
}

static int32_t redisContextTimeoutMsec(const struct timeval *timeout,int32_t *result)
{
	int32_t msec = -1;

	/* Only use timeout when not nullptr. */
	if (timeout != nullptr) 
	{
		if (timeout->tv_usec > 1000000 || timeout->tv_sec > __MAX_MSEC) 
		{
		    *result = msec;
		    return REDIS_ERR;
		}

		msec = (timeout->tv_sec * 1000) + ((timeout->tv_usec + 999) / 1000);
		if (msec < 0 || msec > INT_MAX)	
		{
			msec = INT_MAX; 
		}
	}

    *result = msec;
    return REDIS_OK;
}

int32_t RedisContext::redisContextConnectUnix(const char *path,
		const struct timeval *timeout)
{
	int32_t blocking = (flags & REDIS_BLOCK);
	struct sockaddr_un sa;
	int32_t timeoutMsec = -1;

	Socket socket;
	fd = socket.createSocket();
	if (fd == -1)
    {
        return REDIS_ERR;
    }

	socket.setSocketNonBlock(fd);
	if (this->path != path)
	{
	    this->path = path;
	}

	if (redisContextTimeoutMsec(timeout,&timeoutMsec) != REDIS_OK)
	{
	    return REDIS_ERR;
	}

	sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);

    if (socket.connect(fd,(struct sockaddr*)&sa) == -1)
    {
        if (errno == EINPROGRESS && !blocking) 
        {
            /* This is ok. */
        } 
        else 
        {
            if (redisContextWaitReady(timeoutMsec) != REDIS_OK)
            {
                return REDIS_ERR;
            }
        }
    }

    if(socket.setSocketBlock(fd) == -1)
    {
    	return REDIS_ERR;
    }

	if(socket.setTcpNoDelay(fd,true) == -1)
	{
		return REDIS_ERR;
	}

    flags |= REDIS_CONNECTED;
    return REDIS_OK;
}

int32_t RedisContext::redisContextConnectTcp(const char *ip,
		int16_t port,const struct timeval *timeout)
{
    this->ip = ip;
    this->port = port;

	int32_t timeoutMsec = -1;
	if (timeout != nullptr)
	{
		if (redisContextTimeoutMsec(timeout,&timeoutMsec) != REDIS_OK)
		{
			redisSetError(REDIS_ERR_IO,"Invalid timeout specified");
			return REDIS_ERR;
		 }
	}

	int32_t blocking = (flags & REDIS_BLOCK);
	int32_t reuseaddr = (flags & REDIS_REUSEADDR);
	int32_t reuses = 0;

	Socket socket;
	fd = socket.createSocket();
    if (fd == -1)
    {
        return REDIS_ERR;
    }

    if (!socket.setSocketNonBlock(fd))
    {
        return REDIS_ERR;
    }

	if (socket.connect(fd,ip,port) == -1)
    {
        if (errno == EHOSTUNREACH)
        {
            return REDIS_ERR;
        }
        else if (errno == EINPROGRESS && !blocking)
        {

        }
        else if (errno == EADDRNOTAVAIL && reuseaddr)
        {
            if (++reuses >= REDIS_CONNECT_RETRIES)
            {
                return REDIS_ERR;
            }
            else
            {
                return REDIS_ERR;
            }
        }
        else
        {
            if (redisContextWaitReady(timeoutMsec) != REDIS_OK)
            {
                return REDIS_ERR;
            }
        }
	}

	if (socket.setSocketBlock(fd) == -1)
	{
		return REDIS_ERR;
	}

	if (socket.setTcpNoDelay(fd,true) == -1)
	{
		return REDIS_ERR;
	}
	return REDIS_OK;
}

Hiredis::Hiredis(EventLoop *loop,bool clusterMode)
:pool(loop),
 clusterMode(true)
{
	node = redisAsyncContexts.begin();
}

Hiredis::~Hiredis()
{

}

void Hiredis::clusterMoveConnCallBack(const TcpConnectionPtr &conn)
{
	RedisAsyncCallbackPtr asyncCallback = std::any_cast<RedisAsyncCallbackPtr>(conn->getContext());
	conn->resetContext();
	if (conn->connected())
	{
		RedisAsyncContextPtr ac(new RedisAsyncContext(conn->intputBuffer(),conn));
		insertRedisMap(conn->getSockfd(),ac);
		{
			std::unique_lock<std::mutex> lk(ac->getMutex());
			ac->getAsyncCallback().push_back(asyncCallback);
		}
		conn->sendPipe(asyncCallback->data,asyncCallback->len);
	}
	else
	{
		eraseRedisMap(conn->getSockfd());
	}
}

void Hiredis::clusterAskConnCallBack(const TcpConnectionPtr &conn)
{
	RedisAsyncCallbackPtr asyncCallback = std::any_cast<RedisAsyncCallbackPtr>(conn->getContext());
	conn->resetContext();
	if (conn->connected())
	{
		RedisAsyncContextPtr ac (new RedisAsyncContext(conn->intputBuffer(),conn));
		insertRedisMap(conn->getSockfd(),ac);
		conn->sendPipe("*1\r\n$6\r\nASKING\r\n");
		{
			std::unique_lock<std::mutex> lk(ac->getMutex());
			ac->getAsyncCallback().push_back(asyncCallback);
		}
		conn->sendPipe(asyncCallback->data,asyncCallback->len);
	}
	else
	{
		eraseRedisMap(conn->getSockfd());
	}
}

void Hiredis::redisConnCallBack(const TcpConnectionPtr &conn)
{
	if (conn->connected())
	{
		RedisAsyncContextPtr contextPtr(new RedisAsyncContext(conn->intputBuffer(),conn));
		insertRedisMap(conn->getSockfd(),contextPtr);
		LOG_INFO<<"redis connect "<<conn->getSockfd();
	}
	else
	{
		eraseRedisMap(conn->getSockfd());
		LOG_INFO<<"redis disconnect "<<conn->getSockfd();
	}
}

void Hiredis::clearTcpClient()
{
	std::unique_lock<std::mutex> lk(rtx);
	tcpClients.clear();
	node = redisAsyncContexts.begin();
}

void Hiredis::eraseRedisMap(int32_t sockfd)
{
	std::unique_lock<std::mutex> lk(rtx);
	auto it = redisAsyncContexts.find(sockfd);
	assert(it != redisAsyncContexts.end());
	if (it == node)
	{
		node++;
	}
	redisAsyncContexts.erase(it);
}

void Hiredis::insertRedisMap(int32_t sockfd,const RedisAsyncContextPtr &ac)
{
	std::unique_lock<std::mutex> lk(rtx);
	redisAsyncContexts.insert(std::make_pair(sockfd,ac));
}

void Hiredis::pushTcpClient(const TcpClientPtr &client)
{
	std::unique_lock<std::mutex> lk(rtx);
	tcpClients.push_back(client);
}

RedisAsyncContextPtr Hiredis::getIteratorNode()
{
	std::unique_lock<std::mutex> lk(rtx);

	if (redisAsyncContexts.empty())
	{
		return nullptr;
	}

	if (node == redisAsyncContexts.end())
	{
		node = redisAsyncContexts.begin();
	}

	auto it = node;
	node ++;
	return it->second;
}

void Hiredis::redisReadCallBack(const TcpConnectionPtr &conn,Buffer *buffer)
{
	RedisAsyncContextPtr redisAsyncContextPtr;
	{
		std::unique_lock <std::mutex> lck(rtx);
		auto it = redisAsyncContexts.find(conn->getSockfd());
		assert(it != redisAsyncContexts.end());
		redisAsyncContextPtr = it->second;
	}

	RedisReplyPtr reply;
	int32_t status;
	while((status = redisAsyncContextPtr->redisGetReply(reply)) == REDIS_OK)
	{
		if (reply == nullptr)
		{
			break;
		}

		 if (clusterMode && reply->type == REDIS_REPLY_ERROR &&
				 (!strncmp(reply->str,"MOVED",5) || (!strncmp(reply->str,"ASK",3))))
		 {
			char *p = reply->str,*s;
			int32_t slot;
			s = strchr(p,' ');
			p = strchr(s+1,' ');
			*p = '\0';
			slot = atoi(s+1);
			s = strrchr(p+1,':');
			*s = '\0';

			std::string ip = p + 1;
			int32_t port = atoi(s+1);
			LOG_WARN<<"-> Redirected to slot "<< slot<<" located at "<<ip<<" "<<port;

			RedisAsyncCallbackPtr asyncCb;
			{
				std::unique_lock<std::mutex> lk(redisAsyncContextPtr->getMutex());
				assert(!redisAsyncContextPtr->getAsyncCallback().empty());
				asyncCb = redisAsyncContextPtr->getAsyncCallback().front();
				redisAsyncContextPtr->getAsyncCallback().pop_front();
			}

			TcpClientPtr client(new TcpClient(pool.getNextLoop(),ip.c_str(),port,asyncCb));
			pushTcpClient(client);

			client->setMessageCallback(std::bind(&Hiredis::redisReadCallBack,
					this,std::placeholders::_1,std::placeholders::_2));
			if (!strncmp(reply->str,"MOVED",5))
			{
				client->setConnectionCallback(std::bind(&Hiredis::clusterMoveConnCallBack,
						this,std::placeholders::_1));
			}
			else if (!strncmp(reply->str,"ASK",3))
			{
				client->setConnectionCallback(std::bind(&Hiredis::clusterAskConnCallBack,
						this,std::placeholders::_1));
			}
			client->syncConnect();
		 }
		 else
		 {
			 RedisAsyncCallbackPtr asyncCb;
			 {
				 std::unique_lock<std::mutex> lk(redisAsyncContextPtr->getMutex());
				 assert(!redisAsyncContextPtr->getAsyncCallback().empty());
				 asyncCb = redisAsyncContextPtr->getAsyncCallback().front();
				 redisAsyncContextPtr->getAsyncCallback().pop_front();
			 }

			 if (asyncCb->cb.fn)
			 {
				 asyncCb->cb.fn(redisAsyncContextPtr,reply,asyncCb->cb.privdata);
			 }
		 }
	}
}

RedisContextPtr redisConnectUnix(const char *path)
{
	RedisContextPtr c(new RedisContext());
	c->setBlock();
	c->redisContextConnectUnix(path,nullptr);
	return c;
}

/* Connect to a Redis instance. On error the field error in the returned
 * context will be set to the return value of the error function.
 * When no set of reply functions is given, the default set will be used. */
RedisContextPtr redisConnect(const char *ip,int16_t port)
{
	RedisContextPtr c(new RedisContext());
	c->setBlock();
	if (c->redisContextConnectTcp(ip,port,nullptr) == REDIS_ERR)
	{
		if (!c->err)
		{
			c->redisSetError(REDIS_ERR_IO,nullptr);
		}
	}
	return c;
}

RedisContextPtr redisConnectWithTimeout(const char *ip,int16_t port,const struct timeval tv)
{
	RedisContextPtr c(new RedisContext());
	c->setBlock();

	if (c->redisContextConnectTcp(ip,port,&tv) == REDIS_ERR)
	{
		if (!c->err)
		{
			c->redisSetError(REDIS_ERR_IO,nullptr);
		}
	}
	return c;
}
