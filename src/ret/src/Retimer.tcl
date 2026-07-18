
sta::define_cmd_args "layout_aware_retime" {}

proc layout_aware_retime { args } {
    sta::parse_key_args "layout_aware_retime" args keys {} flags {}

    sta::check_argc_eq0 "layout_aware_retime" $args
    
    ret::layout_aware_retime_cmd
}