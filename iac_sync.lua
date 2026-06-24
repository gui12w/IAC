-- iac_sync.lua (Colocar na pasta de scripts do MPV: %APPDATA%\mpv\scripts\)
local utils = require 'mp.utils'
local iac_file = nil

local STREAMER_PATH = "C:\\Users\\your_pc\\iac_stream.exe" --change the path to where you put the iac_stream

function kill_streamer()
    mp.command_native({
        name = "subprocess",
        args = { "taskkill", "/F", "/IM", "iac_stream.exe" },
        playback_only = false,
        capture_stdout = false,
        capture_stderr = false
    })
end

-- Varre as faixas de áudio e remove especificamente as faixas externas antigas
function remove_external_audio()
    local tracks = mp.get_property_native("track-list", {})
    for _, track in ipairs(tracks) do
        if track.type == "audio" and track.external then
            mp.commandv("audio-remove", track.id)
        end
    end
end

function start_iac_stream(time_pos)
    kill_streamer()

    mp.command_native_async({
        name = "subprocess",
        args = { STREAMER_PATH, iac_file, tostring(time_pos) },
        playback_only = true,
    }, function(success, result, error) end)
end

function attach_iac(path)
    iac_file = path
    mp.msg.info("IAC anexado manualmente: " .. iac_file)
    mp.osd_message("IAC carregado: " .. path, 2)

    local current_time = mp.get_property_number("time-pos", 0)
    remove_external_audio()
    start_iac_stream(current_time)
    
    mp.add_timeout(0.05, function()
        mp.commandv("audio-add", "\\\\.\\pipe\\iac_stream", "select")
    end)
end

function pick_iac_manually()
    if mp.get_property("path") == nil then
        mp.osd_message("Abre um vídeo primeiro.", 2)
        return
    end

    mp.osd_message("Selecione o arquivo .iac...", 5)

    local ps_command = [[
        Add-Type -AssemblyName System.Windows.Forms
        $f = New-Object System.Windows.Forms.OpenFileDialog
        $f.Filter = 'Arquivos IAC (*.iac)|*.iac'
        $f.Title = 'Selecionar arquivo IAC'
        if ($f.ShowDialog() -eq 'OK') { Write-Output $f.FileName }
    ]]

    mp.command_native_async({
        name = "subprocess",
        args = { "powershell", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", ps_command },
        playback_only = false,
        capture_stdout = true,
    }, function(success, result, error)
        if not success or not result or not result.stdout then
            mp.osd_message("Nenhum arquivo selecionado.", 2)
            return
        end

        local chosen_path = result.stdout:gsub("%s+$", "")
        if chosen_path == "" then
            mp.osd_message("Nenhum arquivo selecionado.", 2)
            return
        end

        attach_iac(chosen_path)
    end)
end

mp.add_key_binding("i", "pick-iac-manually", pick_iac_manually)

mp.add_hook("on_load", 50, function()
    local video_path = mp.get_property("stream-open-filename")
    if not video_path then return end

    if video_path:match("%.iac$") then
        mp.osd_message("Use a tecla 'i' para carregar um .iac", 4)
        return
    end

    local potential_iac = video_path:gsub("%.%w+$", ".iac")
    local info = utils.file_info(potential_iac)
    if info and info.is_file then
        iac_file = potential_iac
    else
        iac_file = nil
    end
end)

mp.register_event("file-loaded", function()
    if iac_file then
        mp.add_timeout(0.1, function()
            local current_time = mp.get_property_number("time-pos", 0)
            remove_external_audio()
            start_iac_stream(current_time)
            mp.add_timeout(0.05, function()
                mp.commandv("audio-add", "\\\\.\\pipe\\iac_stream", "select")
            end)
        end)
    end
end)

-- Evento de Seek completamente corrigido e sincronizado
mp.register_event("seek", function()
    if iac_file then
        local current_time = mp.get_property_number("time-pos", 0)
        remove_external_audio() -- Limpa a faixa antiga na hora
        start_iac_stream(current_time)
        
        mp.add_timeout(0.05, function()
            mp.commandv("audio-add", "\\\\.\\pipe\\iac_stream", "select")
        end)
    end
end)

mp.register_event("shutdown", function()
    kill_streamer()
end)