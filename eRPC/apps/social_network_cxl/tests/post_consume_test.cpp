#include "../post_consume.h"

#include <cstdlib>
#include <iostream>
#include <vector>

#include "../social_network_rpc_type.h"

#ifdef SN_TEST_PROTOBUF
#include <social_network.pb.h>
using TestPost = social_network::Post;
#else
struct Person {
  int64_t id = 0;
  std::string name;
  int64_t user_id() const { return id; }
  const std::string &username() const { return name; }
  void set_user_id(int64_t v) { id = v; }
  void set_username(const std::string &v) { name = v; }
};
struct Media {
  int64_t id = 0;
  std::string type;
  int64_t media_id() const { return id; }
  const std::string &media_type() const { return type; }
  void set_media_id(int64_t v) { id = v; }
  void set_media_type(const std::string &v) { type = v; }
};
struct Url {
  std::string shortened, expanded;
  const std::string &shortened_url() const { return shortened; }
  const std::string &expanded_url() const { return expanded; }
  void set_shortened_url(const std::string &v) { shortened = v; }
  void set_expanded_url(const std::string &v) { expanded = v; }
};
struct TestPost {
  int64_t id = 0, request = 0, time = 0;
  int type = 0;
  Person author;
  std::string body;
  std::vector<Media> medias;
  std::vector<Person> mentions;
  std::vector<Url> links;
  int64_t post_id() const { return id; }
  int64_t req_id() const { return request; }
  int64_t timestamp() const { return time; }
  int post_type() const { return type; }
  const Person &creator() const { return author; }
  const std::string &text() const { return body; }
  const std::vector<Media> &media() const { return medias; }
  const std::vector<Person> &user_mentions() const { return mentions; }
  const std::vector<Url> &urls() const { return links; }
  int media_size() const { return static_cast<int>(medias.size()); }
  int user_mentions_size() const { return static_cast<int>(mentions.size()); }
  int urls_size() const { return static_cast<int>(links.size()); }
  void set_post_id(int64_t v) { id = v; }
  void set_req_id(int64_t v) { request = v; }
  void set_timestamp(int64_t v) { time = v; }
  void set_post_type(int v) { type = v; }
  void set_text(const std::string &v) { body = v; }
  Person *mutable_creator() { return &author; }
  Media *add_media() {
    medias.emplace_back();
    return &medias.back();
  }
  Person *add_user_mentions() {
    mentions.emplace_back();
    return &mentions.back();
  }
  Url *add_urls() {
    links.emplace_back();
    return &links.back();
  }
};
#endif

static void require(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

template <class F>
static void require_invalid(F function) {
  bool rejected = false;
  try {
    function();
  } catch (const std::exception &) {
    rejected = true;
  }
  require(rejected, "invalid input accepted");
}

static uint64_t expected_checksum(TestPost &post) {
#ifdef SN_TEST_PROTOBUF
  std::string bytes;
  require(post.SerializeToString(&bytes), "serialization failed");
  TestPost parsed;
  require(parsed.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())),
          "parse failed");
  return sn_consume::protobuf_post(parsed);
#else
  return sn_consume::protobuf_post(post);
#endif
}

static void set_common(PostData &native, TestPost &post,
                       const std::string &text) {
  native.post_id = -7;
  post.set_post_id(-7);
  native.req_id = 123;
  post.set_req_id(123);
  native.timestamp = 987654321;
  post.set_timestamp(native.timestamp);
  native.creator_user_id = 961;
  post.mutable_creator()->set_user_id(961);
  native.creator_username_length =
      set_post_string(native.creator_username, "username_961");
  post.mutable_creator()->set_username("username_961");
  native.text_length = set_post_string(native.text, text);
  post.set_text(text);
}

static void check_post(const std::string &text, bool populated) {
  PostData native;
  native.init();
  TestPost post;
  set_common(native, post, text);

  if (populated) {
    native.post_type = 3;
    post.set_post_type(static_cast<decltype(post.post_type())>(3));

    native.media_count = 1;
    native.media_ids[0] = 77;
    native.media_type_lengths[0] =
        set_post_string(native.media_types[0], "png");
    auto *media = post.add_media();
    media->set_media_id(77);
    media->set_media_type("png");

    native.mentions_count = 1;
    native.mentions_ids[0] = 23;
    native.mentions_username_lengths[0] =
        set_post_string(native.mentions_usernames[0], "user_23");
    auto *mention = post.add_user_mentions();
    mention->set_user_id(23);
    mention->set_username("user_23");

    native.urls_count = 1;
    native.shortened_url_lengths[0] =
        set_post_string(native.shortened_urls[0], "http://s/x");
    native.expanded_url_lengths[0] =
        set_post_string(native.expanded_urls[0], "https://example.org/path");
    auto *url = post.add_urls();
    url->set_shortened_url("http://s/x");
    url->set_expanded_url("https://example.org/path");
  }

  validate_post_data(native);
  const uint64_t expected = expected_checksum(post);
  require(sn_consume::native_post(&native, sizeof(native)) == expected,
          "native/protobuf field mismatch");
  require(sn_consume::native_post_after_full_copy(&native, sizeof(native)) ==
              expected,
          "full-copy native/protobuf field mismatch");

  std::vector<unsigned char> unaligned(sizeof(native) + 1);
  std::memcpy(unaligned.data() + 1, &native, sizeof(native));
  require(
      sn_consume::native_post(unaligned.data() + 1, sizeof(native)) == expected,
      "unaligned input");

  for (size_t i = 0; i < text.size(); ++i) {
    native.text[i] ^= 1;
    require(sn_consume::native_post(&native, sizeof(native)) != expected,
            "text byte not consumed");
    native.text[i] ^= 1;
  }

  native.text[native.text_length] = 'X';
  native.media_ids[SN_MAX_MEDIA - 1] = 333;
  native.mentions_usernames[SN_MAX_MENTIONS - 1][0] = 'X';
  native.expanded_urls[SN_MAX_URLS - 1][0] = 'X';
  require(sn_consume::native_post(&native, sizeof(native)) == expected,
          "unused capacity consumed");

  if (populated) {
    native.mentions_usernames[0][0] ^= 1;
    require(sn_consume::native_post(&native, sizeof(native)) != expected,
            "mention username ignored");
    native.mentions_usernames[0][0] ^= 1;
    native.expanded_urls[0][0] ^= 1;
    require(sn_consume::native_post(&native, sizeof(native)) != expected,
            "URL ignored");
    native.expanded_urls[0][0] ^= 1;
    native.expanded_url_lengths[0] = SN_EXPANDED_URL_LEN;
    require_invalid([&] { sn_consume::native_post(&native, sizeof(native)); });
  }

  native.media_count = SN_MAX_MEDIA + 1;
  require_invalid([&] { sn_consume::native_post(&native, sizeof(native)); });
  native.media_count = 0;
  native.text_length = SN_TEXT_LEN;
  require_invalid([&] { sn_consume::native_post(&native, sizeof(native)); });
  native.text_length = 0;
  native.mentions_count = SN_MAX_MENTIONS + 1;
  require_invalid([&] { sn_consume::native_post(&native, sizeof(native)); });
  native.mentions_count = 0;
  native.object_size = sizeof(native) - 1;
  require_invalid([&] { sn_consume::native_post(&native, sizeof(native)); });
}

static void check_compose() {
  PostData native;
  native.init();
  TestPost post;
  native.creator_user_id = 961;
  post.mutable_creator()->set_user_id(961);
  native.creator_username_length =
      set_post_string(native.creator_username, "username_961");
  post.mutable_creator()->set_username("username_961");

  std::string text(500, 'X');
  std::vector<std::string> names, expanded, shortened;
  for (int i = 0; i < 5; ++i) {
    names.push_back("username_" + std::to_string(900 + i));
    text += "@" + names.back();
    expanded.push_back("http://" + std::string(64, static_cast<char>('A' + i)));
    shortened.push_back("http://short-url/" +
                        std::string(10, static_cast<char>('a' + i)));
  }
  text += " ";
  for (const auto &url : expanded) text += url;
  require(text.size() == 921, "Compose text size differs from generator");
  native.text_length = set_post_string(native.text, text);
  post.set_text(text);

  for (size_t i = 0; i < SN_MAX_MEDIA; ++i) {
    native.media_ids[i] = static_cast<int64_t>(i + 10);
    native.media_type_lengths[i] =
        set_post_string(native.media_types[i], "png");
    auto *media = post.add_media();
    media->set_media_id(static_cast<int64_t>(i + 10));
    media->set_media_type("png");
  }
  native.media_count = SN_MAX_MEDIA;

  for (size_t i = 0; i < SN_MAX_MENTIONS; ++i) {
    native.mentions_ids[i] = static_cast<int64_t>(900 + i);
    native.mentions_username_lengths[i] =
        set_post_string(native.mentions_usernames[i], names[i]);
    auto *mention = post.add_user_mentions();
    mention->set_user_id(static_cast<int64_t>(900 + i));
    mention->set_username(names[i]);
  }
  native.mentions_count = SN_MAX_MENTIONS;

  for (size_t i = 0; i < SN_MAX_URLS; ++i) {
    native.shortened_url_lengths[i] =
        set_post_string(native.shortened_urls[i], shortened[i]);
    native.expanded_url_lengths[i] =
        set_post_string(native.expanded_urls[i], expanded[i]);
    auto *url = post.add_urls();
    url->set_shortened_url(shortened[i]);
    url->set_expanded_url(expanded[i]);
  }
  native.urls_count = SN_MAX_URLS;

  validate_post_data(native);
  require(sn_consume::native_post(&native, sizeof(native)) ==
              expected_checksum(post),
          "complete Compose fields differ");
  require(sn_consume::native_post_after_full_copy(&native, sizeof(native)) ==
              expected_checksum(post),
          "full-copy Compose fields differ");
  require(local_post_string(native.text, native.text_length) == text,
          "Compose text lost");
  std::cout << "Compose initial=500 final_text=" << native.text_length
            << " media=" << native.media_count
            << " mentions=" << native.mentions_count
            << " urls=" << native.urls_count << " total=" << sizeof(native)
            << '\n';
}

int main() {
  check_compose();

  PostData empty;
  empty.init();
  TestPost empty_post;
  RPCMsgReq<PostStorageWriteCXLReq> request(
      RPC_TYPE::RPC_COMPOSE_POST_WRITE_REQ, 1, {0, empty});
  require(request.req_control.post.object_size == sizeof(PostData),
          "request layout");
  require(sn_consume::native_post(&empty, sizeof(empty)) ==
              sn_consume::protobuf_post(empty_post),
          "empty/default mismatch");
  require(sn_consume::native_post_after_full_copy(&empty, sizeof(empty)) ==
              sn_consume::protobuf_post(empty_post),
          "full-copy empty/default mismatch");

  for (size_t length : {size_t{0}, size_t{1}, size_t{7}, size_t{8}, size_t{63},
                        size_t{64}, size_t{65}, size_t{150}, SN_TEXT_LEN - 1}) {
    std::string text(length, 'a');
    if (length > 8) text[7] = '\0';
    check_post(text, false);
    check_post(text, true);
  }
  check_post("\xe4\xb8\xad\xe6\x96\x87", true);

  require_invalid(
      [&] { set_post_string(empty.text, std::string(SN_TEXT_LEN, 'x')); });
  require_invalid([&] { sn_consume::native_post(&empty, sizeof(empty) - 1); });
  require_invalid([&] { sn_consume::native_post(nullptr, sizeof(empty)); });
  require_invalid([&] {
    sn_consume::native_post_after_full_copy(&empty, sizeof(empty) - 1);
  });
  require_invalid([&] {
    sn_consume::native_post_after_full_copy(nullptr, sizeof(empty));
  });

  std::cout << "post_consume_test PASS sizeof(PostData)=" << sizeof(PostData)
            << '\n';
}
