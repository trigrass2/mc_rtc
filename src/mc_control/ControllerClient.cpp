#include <mc_control/ControllerClient.h>
#include <mc_rtc/GUIState.h>
#include <mc_rtc/logging.h>

#include <nanomsg/nn.h>
#include <nanomsg/pipeline.h>
#include <nanomsg/pubsub.h>

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace
{

std::string cat2str(const std::vector<std::string> & cat)
{
  std::string ret;
  for(size_t i = 0; i < cat.size(); ++i)
  {
    ret += cat[i];
    if(i != cat.size() - 1)
    {
      ret += "/";
    }
  }
  return ret;
}

} // namespace

namespace mc_control
{

ControllerClient::ControllerClient(const std::string & sub_conn_uri, const std::string & push_conn_uri, double timeout)
: timeout_(timeout)
{
  auto init_socket = [](int & socket, unsigned int proto, const std::string & uri, const std::string & name) {
    socket = nn_socket(AF_SP, proto);
    if(socket < 0)
    {
      LOG_ERROR_AND_THROW(std::runtime_error, "Failed to initialize " << name)
    }
    int ret = nn_connect(socket, uri.c_str());
    if(ret < 0)
    {
      LOG_ERROR_AND_THROW(std::runtime_error, "Failed to connect " << name << " to uri: " << uri)
    }
    else
    {
      LOG_INFO("Connected " << name << " to " << uri)
    }
  };
  init_socket(sub_socket_, NN_SUB, sub_conn_uri, "SUB socket");
  int err = nn_setsockopt(sub_socket_, NN_SUB, NN_SUB_SUBSCRIBE, "", 0);
  if(err < 0)
  {
    LOG_ERROR_AND_THROW(std::runtime_error, "Failed to set subscribe option on SUB socket")
  }
  init_socket(push_socket_, NN_PUSH, push_conn_uri, "PUSH socket");
}

ControllerClient::~ControllerClient()
{
  run_ = false;
  if(sub_th_.joinable())
  {
    sub_th_.join();
  }
  nn_shutdown(sub_socket_, 0);
  nn_shutdown(push_socket_, 0);
}

void ControllerClient::start()
{
  sub_th_ = std::thread([this]() {
    std::vector<char> buff(65536);
    auto t_last_received = std::chrono::system_clock::now();
    while(run_)
    {
      memset(buff.data(), 0, buff.size() * sizeof(char));
      auto recv = nn_recv(sub_socket_, buff.data(), buff.size(), NN_DONTWAIT);
      auto now = std::chrono::system_clock::now();
      if(recv < 0)
      {
        if(timeout_ > 0 && now - t_last_received > std::chrono::duration<double>(timeout_))
        {
          t_last_received = now;
          if(run_)
          {
            handle_gui_state("{}");
          }
        }
        auto err = nn_errno();
        if(err != EAGAIN)
        {
          LOG_ERROR("ControllerClient failed to receive with errno: " << err)
        }
      }
      else if(recv > 0)
      {
        if(recv > static_cast<int>(buff.size()))
        {
          LOG_WARNING("Receive buffer was too small to receive the latest state message, will resize for next time")
          buff.resize(2 * buff.size());
          continue;
        }
        t_last_received = now;
        if(run_)
        {
          handle_gui_state(buff.data());
        }
      }
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
  });
}

void ControllerClient::send_request(const ElementId & id, const mc_rtc::Configuration & data)
{
  mc_rtc::Configuration request;
  request.add("category", id.category);
  request.add("name", id.name);
  request.add("data", data);
  std::string out = request.dump();
  nn_send(push_socket_, out.c_str(), out.size() + 1, NN_DONTWAIT);
}

void ControllerClient::send_request(const ElementId & id)
{
  send_request(id, mc_rtc::Configuration{});
}

void ControllerClient::handle_gui_state(const char * data)
{
  started();
  auto state = mc_rtc::Configuration::fromData(data);
  if(state.has("DATA"))
  {
    data_.load(state("DATA"));
    state.remove("DATA");
  }
  else
  {
    data_ = mc_rtc::Configuration{};
  }
  handle_category({}, "", state);
  stopped();
}

void ControllerClient::handle_category(const std::vector<std::string> & parent,
                                       const std::string & category,
                                       const mc_rtc::Configuration & data)
{
  if(category.size())
  {
    this->category(parent, category);
  }
  auto next_category = parent;
  if(category.size())
  {
    next_category.push_back(category);
  }
  for(const auto & k : data.keys())
  {
    auto d = data(k);
    if(d.has("GUI"))
    {
      handle_widget({next_category, k}, d);
    }
    else
    {
      handle_category(next_category, k, d);
    }
  }
}

void ControllerClient::handle_widget(const ElementId & id, const mc_rtc::Configuration & data)
{
  auto gui = data("GUI");
  if(!gui.has("type"))
  {
    LOG_ERROR("GUI entry " << id.name << " in category " << cat2str(id.category) << " has no type")
    return;
  }
  auto type = static_cast<mc_rtc::gui::Elements>(static_cast<int>(gui("type")));
  try
  {
    using Elements = mc_rtc::gui::Elements;
    switch(type)
    {
      case Elements::Label:
        label(id, std::string{data("data", data("data").dump())});
        break;
      case Elements::ArrayLabel:
        array_label(id, gui("labels", std::vector<std::string>{}), data("data"));
        break;
      case Elements::Button:
        button(id);
        break;
      case Elements::Checkbox:
        checkbox(id, data("data"));
        break;
      case Elements::StringInput:
        string_input(id, data("data"));
        break;
      case Elements::IntegerInput:
        integer_input(id, data("data"));
        break;
      case Elements::NumberInput:
        number_input(id, data("data"));
        break;
      case Elements::NumberSlider:
        number_slider(id, data("data"), gui("min"), gui("max"));
        break;
      case Elements::ArrayInput:
        array_input(id, gui("labels", std::vector<std::string>{}), data("data"));
        break;
      case Elements::ComboInput:
        combo_input(id, gui("values"), data("data"));
        break;
      case Elements::DataComboInput:
        data_combo_input(id, gui("ref"), data("data"));
        break;
      case Elements::Point3D:
        handle_point3d(id, gui, data);
        break;
      case Elements::Point3DTrajectory:
        handle_point3DTrajectory(id, gui, data);
        break;
      case Elements::PoseTrajectory:
        handle_poseTrajectory(id, gui, data);
        break;
      case Elements::Polygon:
        handle_polygon(id, gui, data);
        break;
      case Elements::Force:
        handle_force(id, gui, data);
        break;
      case Elements::Arrow:
        handle_arrow(id, gui, data);
        break;
      case Elements::Rotation:
        handle_rotation(id, gui, data);
        break;
      case Elements::Transform:
        handle_transform(id, gui, data);
        break;
      case Elements::Schema:
        schema(id, gui("dir"));
        break;
      case Elements::Form:
        handle_form(id, gui("form"));
        break;
      default:
        LOG_ERROR("Type " << static_cast<int>(type) << " is not handlded by this ControllerClient")
        break;
    };
  }
  catch(const mc_rtc::Configuration::Exception & exc)
  {
    LOG_ERROR("Deserialization of GUI entry " << id.name << " in category " << cat2str(id.category) << " went wrong...")
    LOG_WARNING("Data was: " << std::endl << data.dump(true))
    LOG_WARNING("mc_rtc::Configuration exception was: " << std::endl << exc.what())
  }
}

void ControllerClient::default_impl(const std::string & type, const ElementId & id)
{
  LOG_WARNING("This implementation of ControllerClient does not handle " << type << " GUI needed by "
                                                                         << cat2str(id.category) << "/" << id.name)
}

void ControllerClient::handle_point3d(const ElementId & id,
                                      const mc_rtc::Configuration & gui,
                                      const mc_rtc::Configuration & data)
{
  bool ro = gui("ro", false);
  Eigen::Vector3d pos = data("data");
  if(ro)
  {
    array_label(id, {"x", "y", "z"}, pos);
  }
  else
  {
    array_input(id, {"x", "y", "z"}, pos);
  }
  point3d({id.category, id.name + "_point3d"}, id, ro, pos);
}

void ControllerClient::handle_point3DTrajectory(const ElementId & id,
                                                const mc_rtc::Configuration & /* gui */,
                                                const mc_rtc::Configuration & data)
{
  const std::vector<Eigen::Vector3d> & points = data("data");
  trajectory(id, points);
}

void ControllerClient::handle_poseTrajectory(const ElementId & id,
                                             const mc_rtc::Configuration & /* gui */,
                                             const mc_rtc::Configuration & data)
{
  const std::vector<sva::PTransformd> & points = data("data");
  trajectory(id, points);
}

void ControllerClient::handle_polygon(const ElementId & id,
                                      const mc_rtc::Configuration & gui,
                                      const mc_rtc::Configuration & data)
{
  const std::vector<Eigen::Vector3d> & points = data("data");
  const mc_rtc::gui::Color & color = gui("color");
  polygon(id, points, color);
}

void ControllerClient::handle_force(const ElementId & id,
                                    const mc_rtc::Configuration & gui,
                                    const mc_rtc::Configuration & data)
{
  const sva::ForceVecd & force_ = data("force");
  const sva::PTransformd & surface = data("surface");
  const mc_rtc::gui::ForceConfig & forceConfig = gui("config");
  array_label(id, {"cx", "cy", "cz", "fx", "fy", "fz"}, force_.vector());
  force({id.category, id.name + "_force"}, id, force_, surface, forceConfig);
}

void ControllerClient::handle_arrow(const ElementId & id,
                                    const mc_rtc::Configuration & gui,
                                    const mc_rtc::Configuration & data)
{
  const Eigen::Vector3d & arrow_start = data("start");
  const Eigen::Vector3d & arrow_end = data("end");
  const mc_rtc::gui::ArrowConfig & arrow_config = gui("config");
  arrow(id, arrow_start, arrow_end, arrow_config);
}

void ControllerClient::handle_rotation(const ElementId & id,
                                       const mc_rtc::Configuration & gui,
                                       const mc_rtc::Configuration & data)
{
  bool ro = gui("ro", false);
  sva::PTransformd pos = data("data");
  Eigen::Quaterniond q{pos.rotation()};
  if(ro)
  {
    array_label(id, {"w", "x", "y", "z"}, Eigen::Vector4d{q.w(), q.x(), q.y(), q.z()});
  }
  else
  {
    array_input(id, {"w", "x", "y", "z"}, Eigen::Vector4d{q.w(), q.x(), q.y(), q.z()});
  }
  rotation({id.category, id.name + "_rotation"}, id, ro, pos);
}

void ControllerClient::handle_transform(const ElementId & id,
                                        const mc_rtc::Configuration & gui,
                                        const mc_rtc::Configuration & data)
{
  bool ro = gui("ro", false);
  sva::PTransformd pos = data("data");
  Eigen::Quaterniond q{pos.rotation()};
  Eigen::Matrix<double, 7, 1> vec;
  vec << q.w(), q.x(), q.y(), q.z(), pos.translation();
  if(ro)
  {
    array_label(id, {"qw", "qx", "qy", "qz", "tx", "ty", "tz"}, vec);
  }
  else
  {
    array_input(id, {"qw", "qx", "qy", "qz", "tx", "ty", "tz"}, vec);
  }
  transform({id.category, id.name + "_transform"}, id, ro, pos);
}

void ControllerClient::handle_form(const ElementId & id, const mc_rtc::Configuration & gui)
{
  form(id);
  for(const auto & k : gui.keys())
  {
    auto el = gui(k);
    auto type = static_cast<mc_rtc::gui::Elements>(static_cast<int>(el("type")));
    bool required = el("required", false);
    using Elements = mc_rtc::gui::Elements;
    switch(type)
    {
      case Elements::Checkbox:
        form_checkbox(id, k, required, el("default"));
        break;
      case Elements::IntegerInput:
        form_integer_input(id, k, required, el("default"));
        break;
      case Elements::NumberInput:
        form_number_input(id, k, required, el("default"));
        break;
      case Elements::StringInput:
        form_string_input(id, k, required, el("default"));
        break;
      case Elements::ArrayInput:
        form_array_input(id, k, required, el("default"), el("fixed_size"));
        break;
      case Elements::ComboInput:
        form_combo_input(id, k, required, el("values"), el("send_index", false));
        break;
      case Elements::DataComboInput:
        form_data_combo_input(id, k, required, el("ref"), el("send_index", false));
        break;
      default:
        LOG_ERROR("Form cannot handle element of type " << static_cast<int>(type))
    }
  }
}

} // namespace mc_control