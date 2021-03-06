#include "client.hpp"

#include "ircsplit.hpp"
#include "ircd.hpp"
#include "tokens.hpp"
#include <cassert>
//#define PRINT_CLIENT_MESSAGE

void Client::reset_to(Connection conn)
{
  // this resets the client to a new connection
  // regis field is 1, which means there is a connection
  this->regis = 1;
  this->umodes_ = default_user_modes();
  this->conn = conn;
  this->nick_.clear();
  this->user_.clear();
  this->host_.clear();
  this->channels_.clear();
  this->buffer.clear();
  
  // send auth notices
  auth_notice();
}
void Client::disable()
{
  assert(is_alive());
  // release current nickname (if any)
  if (!nick().empty())
    server.erase_nickname(nick());
  // a few should not happens
  nick_ = "BUG_BUG_BUG";
  user_ = "BUG_BUG_BUG";
  host_ = "BUG_BUG.BUG";
  conn = nullptr;
  // reset client status
  regis = 0;
  // free client on server
  server.free_client(*this);
}

#include <kernel/syscalls.hpp>
void Client::split_message(const std::string& msg)
{
  // in case splitter is bad
  SET_CRASH_CONTEXT("Client::split_message():\n'%s'", msg.c_str());
  
  std::string source;
  auto vec = split(msg, source);
  
#ifdef PRINT_CLIENT_MESSAGE
  printf("[Client]: ");
  for (auto& str : vec)
  {
    printf("[%s]", str.c_str());
  }
  printf("\n");
#endif
  
  // ignore empty messages
  if (vec.empty()) return;
  // transform command to uppercase
  extern void transform_to_upper(std::string& str);
  transform_to_upper(vec[0]);
  // handle message
  assert(is_alive());
  if (this->is_reg() == false)
    handle_new(source, vec);
  else
    handle(source, vec);
}

void Client::read(const uint8_t* buf, size_t len)
{
  // in case parser is bad, set context string early
  SET_CRASH_CONTEXT("Client::read():\n\t'%*s'", len, buf);
  
  while (len > 0) {
    
    int search = -1;
    
    // find line ending
    for (size_t i = 0; i < len; i++)
    if (buf[i] == 13 || buf[i] == 10) {
      search = i; break;
    }
    
    // not found:
    if (search == -1)
    {
      // append entire buffer
      buffer.append((char*) buf, len);
      break;
    }
    else {
      // found CR LF:
      if (search != 0) {
        // append to clients buffer
        buffer.append((char*) buf, search);
  
        // move forward in socket buffer
        buf += search;
        // decrease len
        len -= search;
      }
      else {
        buf++; len--;
      }
  
      // parse message
      if (buffer.size())
      {
        split_message(buffer);
        buffer.clear();
      }
    }
  }
}

void Client::send_from(const std::string& from, uint16_t numeric, const std::string& text)
{
  char data[128];
  int len = snprintf(data, 127,
    ":%s %03u %s\r\n", from.c_str(), numeric, text.c_str());
  
  conn->write(data, len);
}
void Client::send_nonick(uint16_t numeric, const std::string& text)
{
  send_from(server.name(), numeric, text);
}
void Client::send(std::string text)
{
  char data[128];
  int len = snprintf(data, 127,
    ":%s %s\r\n", server.name().c_str(), text.c_str());
  
  conn->write(data, len);
}
void Client::send_raw(std::string text)
{
  text += "\r\n";
  //printf("-> %s", text.c_str());
  conn->write(text.c_str(), text.size());
}
void Client::send_raw(const char* text, size_t len)
{
  //printf("-> %s\n", text.c_str());
  conn->write(text, len);
}

// validate name, returns false if invalid characters
static bool validate_name(const std::string& new_name)
{
  // empty nickname is invalid
  if (new_name.empty()) return false;
  // forbidden first characters
  if (isdigit(new_name[0])) return false;
  // a-z A-Z 0-9 _ - \ [ ] { } ^ ` |
  static const std::string LUT = 
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890_-\\[]{}^`|ÆØÅæøå";
  // forbidden characters
  if (LUT.find_first_of(new_name) == std::string::npos) return false;
  return true;
}
bool Client::change_nick(const std::string& new_nick)
{
  if (new_nick.size() < server.nick_minlen()) {
    send_nonick(ERR_ERRONEUSNICKNAME, new_nick + " :Nickname too short");
    return false;
  }
  if (new_nick.size() > server.nick_maxlen()) {
    send_nonick(ERR_ERRONEUSNICKNAME, new_nick + " :Nickname too long");
    return false;
  }
  if (!validate_name(new_nick)) {
    send_nonick(ERR_ERRONEUSNICKNAME, new_nick + " :Erroneous nickname");
    return false;
  }
  auto idx = server.user_by_name(new_nick);
  if (idx != NO_SUCH_CLIENT) {
    send_nonick(ERR_NICKNAMEINUSE, new_nick + " :Nickname is already in use");
    return false;
  }
  // remove old nickname from hashtable
  if (!nick().empty())
      server.erase_nickname(nick());
  // store new nickname
  server.hash_nickname(new_nick, get_id());
  // nickname is valid and free, take it
  this->nick_ = new_nick;
  return true;
}

std::string Client::mode_string() const
{
  std::string res;
  res.reserve(4);
  
  for (int i = 0; i < 8; i++)
  {
    if (umodes_ & (1 << i))
        res += usermodes.bit_to_char(i);
  }
  return res;
}

void Client::handle_quit(const std::string& reason)
{
  if (is_reg()) {
    // inform others about disconnect
    char buffer[256];
    int len = snprintf(buffer, sizeof(buffer),
              ":%s QUIT :%s\r\n", nickuserhost().c_str(), reason.c_str());
    server.user_bcast_butone(get_id(), buffer, len);
    // remove client from various lists
    for (size_t idx : channels()) {
      server.get_channel(idx).remove(get_id());
    }
  }
}
