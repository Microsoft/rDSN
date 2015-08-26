/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
# include "net_io.h"

# ifdef __TITLE__
# undef __TITLE__
# endif
# define __TITLE__ "net.boost.asio"

namespace dsn {
    namespace tools {

        net_io::net_io(
            const dsn_address_t& remote_addr,
            boost::asio::ip::tcp::socket& socket,
            std::shared_ptr<dsn::message_parser>& parser,
            boost::asio::io_service& ios
            )
            :
            _io_service(ios),
            _socket(std::move(socket)),
            _remote_address(remote_addr),
            _parser(parser)
        {
            set_options();
        }

        net_io::~net_io()
        {
        }

        void net_io::set_options()
        {
            if (_socket.is_open())
            {
                try {
                    boost::asio::socket_base::send_buffer_size option, option2(16 * 1024 * 1024);
                    _socket.get_option(option);
                    int old = option.value();
                    _socket.set_option(option2);
                    _socket.get_option(option);

                    /*ddebug("boost asio send buffer size is %u, set as 16MB, now is %u",
                    old, option.value());*/

                    boost::asio::socket_base::receive_buffer_size option3, option4(16 * 1024 * 1024);
                    _socket.get_option(option3);
                    old = option3.value();
                    _socket.set_option(option4);
                    _socket.get_option(option3);
                    /*ddebug("boost asio recv buffer size is %u, set as 16MB, now is %u",
                    old, option.value());*/
                }
                catch (std::exception& ex)
                {
                    dwarn("network session %x:%hu set socket option failed, err = %s",
                        _remote_address.ip,
                        _remote_address.port,
                        ex.what()
                        );
                }
            }
        }

        void net_io::close()
        {
            try {
                _socket.shutdown(boost::asio::socket_base::shutdown_type::shutdown_both);
            }
            catch (std::exception& ex)
            {
                ex;
                /*dwarn("network session %s:%hu exits failed, err = %s",
                    _remote_address.to_ip_string().c_str(),
                    static_cast<int>_remote_address.port,
                    ex.what()
                    );*/
            }

            _socket.close();
        }

        void net_io::do_read(size_t sz)
        {
            add_reference();

            void* ptr = _parser->read_buffer_ptr((int)sz);
            int remaining = _parser->read_buffer_capacity();

            _socket.async_read_some(boost::asio::buffer(ptr, remaining),
                [this](boost::system::error_code ec, std::size_t length)
            {
                if (!!ec)
                {
                    on_failure();
                }
                else
                {
                    int read_next;
                    message_ex* msg = _parser->get_message_on_receive((int)length, read_next);

                    while (msg != nullptr)
                    {
                        this->on_message_read(msg);
                        msg = _parser->get_message_on_receive(0, read_next);
                    }
                     
                    do_read(read_next);
                }

                release_reference();
            });
        }
        
        void net_io::write(message_ex* msg)
        {
            // make sure header is already in the buffer
            // make sure header is already in the buffer
            int tlen;
            int buffer_count = _parser->get_send_buffers_count_and_total_length(msg, &tlen);
            auto buffers = (dsn_message_parser::send_buf*)alloca(buffer_count * sizeof(dsn_message_parser::send_buf));

            int c = _parser->prepare_buffers_on_send(msg, 0, buffers);
            
            std::vector<boost::asio::const_buffer> buffers2;
            buffers2.resize(c);

            for (int i = 0; i < c; i++)
            {
                buffers2[i] = boost::asio::const_buffer(buffers[i].buf, buffers[i].sz);
            }

            add_reference();
            boost::asio::async_write(_socket, buffers2,
                [this, msg](boost::system::error_code ec, std::size_t length)
            {
                if (!!ec)
                {
                    on_failure();
                }
                else
                {
                    on_write_completed(msg);
                }

                release_reference();
            });
        }
        

    }
}